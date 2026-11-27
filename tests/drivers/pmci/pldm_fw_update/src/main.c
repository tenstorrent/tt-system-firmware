/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Drive the DSP0267 firmware device through whole updates, with this file
 * playing the update agent, against the flash simulator's MCUboot slots.
 */

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <libpldm/base.h>
#include <libpldm/firmware_update.h>

#include <string.h>

#include "pldm_fw_update.h"

#define UA_EID 8U

#define SLOT0 PARTITION_ID(slot0_partition)
#define SLOT1 PARTITION_ID(slot1_partition)

/* Where the update image starts in slot1: the second sector with swap using offset, else 0 */
static size_t slot1_img_offset;

/*
 * libpldm's firmware device asks for exactly the bytes left in the last chunk,
 * and the update agent's decoder (libpldm, as in pldmd) refuses requests under
 * 32 bytes, so packages are padded to the transfer size. The image here is a
 * multiple of 32 bytes but not of the 512-byte flash write buffer.
 */
#define IMG_HDR_SIZE   0x200U
#define IMG_BODY_SIZE  6000U
#define IMG_TLV_SIZE   112U
#define IMG_TOTAL_SIZE (IMG_HDR_SIZE + IMG_BODY_SIZE + IMG_TLV_SIZE)
BUILD_ASSERT(IMG_TOTAL_SIZE % 32U == 0U && IMG_TOTAL_SIZE % 512U != 0U);

#define FWU_NODE            DT_COMPAT_GET_ANY_STATUS_OKAY(tenstorrent_pldm_fw_update)
#define COMP_CLASSIFICATION PLDM_COMP_FIRMWARE
#define COMP_ID             DT_PROP(FWU_NODE, component_id)

/* From boards/native_sim.overlay */
static const uint8_t expected_uuid[PLDM_FWUP_UUID_LENGTH] = {
	0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0x4d, 0xef,
	0x81, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};

/* bootutil_public.c, linked in for MCUBOOT_IMG_MANAGER, declares these in a private header */
int boot_write_magic(const struct flash_area *fap);
int boot_write_copy_done(const struct flash_area *fap);

#define MSG_MAX 300U

static uint8_t img[IMG_TOTAL_SIZE];
static uint8_t ua_instance_id;
static uint8_t sink[MSG_MAX];

PLDM_MSG_BUFFER(req_buf, MSG_MAX);
PLDM_MSG_BUFFER(resp_buf, MSG_MAX);
PLDM_MSG_BUFFER(fd_buf, MSG_MAX);

static struct pldm_msg *const req = (struct pldm_msg *)req_buf;
static struct pldm_msg *const resp = (struct pldm_msg *)resp_buf;
static struct pldm_msg *const fd_req = (struct pldm_msg *)fd_buf;

static const char ver_b[] = "0.33.111+0";

static uint8_t next_iid(void)
{
	ua_instance_id = (ua_instance_id + 1U) % 32U;
	return ua_instance_id;
}

static void make_image(uint8_t *buf, uint8_t major, uint8_t minor, uint16_t revision)
{
	memset(buf, 0, IMG_TOTAL_SIZE);
	sys_put_le32(0x96f3b83dU, &buf[0]);
	sys_put_le16(IMG_HDR_SIZE, &buf[8]);
	sys_put_le32(IMG_BODY_SIZE, &buf[12]);
	buf[20] = major;
	buf[21] = minor;
	sys_put_le16(revision, &buf[22]);
	for (size_t i = IMG_HDR_SIZE; i < IMG_TOTAL_SIZE; i++) {
		buf[i] = (uint8_t)(i * 7U + 3U);
	}
}

static const struct flash_area *open_area(uint8_t id)
{
	const struct flash_area *fa;

	zassert_ok(flash_area_open(id, &fa));
	return fa;
}

/* Send a UA request to the FD; returns the response payload length */
static size_t ua_request(size_t payload_len)
{
	size_t len = sizeof(struct pldm_msg_hdr) + payload_len;
	size_t resp_len = MSG_MAX;

	zassert_ok(tt_pldm_fw_update_handle_msg(UA_EID, req, len, resp, &resp_len));
	zassert_true(resp_len >= sizeof(struct pldm_msg_hdr) + 1U, "no response");
	return resp_len - sizeof(struct pldm_msg_hdr);
}

/* Answer an FD request; the FD sends nothing back */
static void ua_respond(size_t payload_len)
{
	size_t len = sizeof(struct pldm_msg_hdr) + payload_len;
	size_t out_len = MSG_MAX;

	zassert_ok(tt_pldm_fw_update_handle_msg(UA_EID, resp, len, sink, &out_len));
	zassert_equal(out_len, 0U);
}

/* Run progress once; returns the FD's request payload length, or 0 when none */
static size_t fd_progress(struct pldm_header_info *hdr)
{
	size_t len = MSG_MAX;
	uint8_t eid = 0;

	zassert_ok(tt_pldm_fw_update_progress(fd_req, &len, &eid));
	if (len == 0U) {
		return 0;
	}
	zassert_equal(eid, UA_EID);
	zassert_equal(unpack_pldm_header(&fd_req->hdr, hdr), PLDM_SUCCESS);
	zassert_equal(hdr->msg_type, PLDM_REQUEST);
	zassert_equal(hdr->pldm_type, PLDM_FWUP);
	return len - sizeof(struct pldm_msg_hdr);
}

static void get_fw_params(char *active, char *pending)
{
	struct pldm_get_firmware_parameters_resp params;
	struct variable_field a, p, table;
	size_t len;

	zassert_ok(encode_get_firmware_parameters_req(next_iid(), 0, req));
	len = ua_request(0);
	zassert_ok(decode_get_firmware_parameters_resp(resp, len, &params, &a, &p, &table));
	zassert_equal(params.completion_code, PLDM_SUCCESS);
	zassert_equal(params.comp_count, 1U);

	memcpy(active, a.ptr, a.length);
	active[a.length] = '\0';
	memcpy(pending, p.ptr, p.length);
	pending[p.length] = '\0';
}

static void ua_request_update(void)
{
	static const char set_ver[] = "dmc-test";
	const struct variable_field v = {
		.ptr = (const uint8_t *)set_ver,
		.length = strlen(set_ver),
	};
	uint8_t cc, will_send;
	uint16_t meta_len;
	size_t len;

	zassert_ok(encode_request_update_req(next_iid(), 512, 1, 1, 0, PLDM_STR_TYPE_ASCII,
					     v.length, &v, req,
					     sizeof(struct pldm_request_update_req) + v.length));
	len = ua_request(sizeof(struct pldm_request_update_req) + v.length);
	zassert_ok(decode_request_update_resp(resp, len, &cc, &meta_len, &will_send));
	zassert_equal(cc, PLDM_SUCCESS);
}

static uint8_t ua_pass_component(uint16_t comp_id)
{
	const struct variable_field v = {.ptr = (const uint8_t *)ver_b, .length = strlen(ver_b)};
	size_t payload_len = sizeof(struct pldm_pass_component_table_req) + v.length;
	uint8_t cc, comp_resp, comp_resp_code;
	size_t len;

	zassert_ok(encode_pass_component_table_req(
		next_iid(), PLDM_START_AND_END, COMP_CLASSIFICATION, comp_id, 0, 0,
		PLDM_STR_TYPE_ASCII, v.length, &v, req, payload_len));
	len = ua_request(payload_len);
	zassert_ok(decode_pass_component_table_resp(resp, len, &cc, &comp_resp, &comp_resp_code));
	zassert_equal(cc, PLDM_SUCCESS);
	return comp_resp_code;
}

/* Returns the component compatibility response code */
static uint8_t ua_update_component_code(uint32_t size)
{
	const struct variable_field v = {.ptr = (const uint8_t *)ver_b, .length = strlen(ver_b)};
	size_t payload_len = sizeof(struct pldm_update_component_req) + v.length;
	bitfield32_t flags = {.value = 0};
	uint8_t cc, compat, compat_code;
	uint16_t time_before;
	size_t len;

	zassert_ok(encode_update_component_req(next_iid(), COMP_CLASSIFICATION, COMP_ID, 0, 0, size,
					       flags, PLDM_STR_TYPE_ASCII, v.length, &v, req,
					       payload_len));
	len = ua_request(payload_len);
	zassert_ok(decode_update_component_resp(resp, len, &cc, &compat, &compat_code, &flags,
						&time_before));
	zassert_equal(cc, PLDM_SUCCESS);
	zassert_equal(compat == PLDM_CCR_COMP_CAN_BE_UPDATED,
		      compat_code == PLDM_CCRC_NO_RESPONSE_CODE, "compat %u code %u", compat,
		      compat_code);
	return compat_code;
}

static void ua_update_component(uint32_t size)
{
	zassert_equal(ua_update_component_code(size), PLDM_CCRC_NO_RESPONSE_CODE);
}

static void ua_cancel_update(void)
{
	uint8_t cc;
	bool8_t non_functioning;
	bitfield64_t bitmap;
	size_t len;

	zassert_ok(encode_cancel_update_req(next_iid(), req, PLDM_CANCEL_UPDATE_REQ_BYTES));
	len = ua_request(PLDM_CANCEL_UPDATE_REQ_BYTES);
	zassert_ok(decode_cancel_update_resp(resp, len, &cc, &non_functioning, &bitmap));
	zassert_equal(cc, PLDM_SUCCESS);
}

static uint8_t ua_activate_with(bool self_contained)
{
	uint8_t cc;
	uint16_t est;
	size_t len;

	zassert_ok(encode_activate_firmware_req(next_iid(), self_contained, req,
						sizeof(struct pldm_activate_firmware_req)));
	len = ua_request(sizeof(struct pldm_activate_firmware_req));
	if (len == 1U) {
		/* Completion code only */
		return resp->payload[0];
	}
	zassert_ok(decode_activate_firmware_resp(resp, len, &cc, &est));
	return cc;
}

static uint8_t ua_activate(void)
{
	return ua_activate_with(false);
}

/*
 * Serve RequestFirmwareData until the FD reports TransferComplete, or until
 * max_chunks chunks have been served. Returns the TransferComplete result, or
 * 0xff when stopped early.
 */
static uint8_t ua_serve_image(const uint8_t *image, uint32_t size, int max_chunks)
{
	struct pldm_header_info hdr;
	uint32_t offset, length;
	uint8_t result;
	size_t len;
	int chunks = 0;

	for (;;) {
		len = fd_progress(&hdr);
		zassert_not_equal(len, 0U, "FD sent nothing");

		if (hdr.command == PLDM_TRANSFER_COMPLETE) {
			zassert_ok(decode_transfer_complete_req(fd_req, len, &result));
			zassert_ok(
				encode_transfer_complete_resp(hdr.instance, PLDM_SUCCESS, resp, 1));
			ua_respond(1);
			return result;
		}

		zassert_equal(hdr.command, PLDM_REQUEST_FIRMWARE_DATA);
		if (chunks++ == max_chunks) {
			return 0xff;
		}
		zassert_ok(decode_request_firmware_data_req(fd_req, len, &offset, &length));
		zassert_true(offset + length <= size, "request past the end");
		zassert_true(length <= CONFIG_TT_PMCI_PLDM_FW_UPDATE_TRANSFER_SIZE);
		zassert_ok(encode_request_firmware_data_resp(hdr.instance, PLDM_SUCCESS, resp,
							     1 + length));
		memcpy(&resp->payload[1], &image[offset], length);
		ua_respond(1 + length);
	}
}

/* Answer VerifyComplete or ApplyComplete; returns the FD's result */
static uint8_t ua_answer_complete(uint8_t command)
{
	struct pldm_header_info hdr;
	bitfield16_t mods;
	uint8_t result;
	size_t len;

	len = fd_progress(&hdr);
	zassert_equal(hdr.command, command);

	if (command == PLDM_VERIFY_COMPLETE) {
		zassert_ok(decode_verify_complete_req(fd_req, len, &result));
		zassert_ok(encode_verify_complete_resp(hdr.instance, PLDM_SUCCESS, resp, 1));
	} else {
		zassert_ok(decode_apply_complete_req(fd_req, len, &result, &mods));
		zassert_ok(encode_apply_complete_resp(hdr.instance, PLDM_SUCCESS, resp, 1));
	}
	ua_respond(1);
	return result;
}

/* A whole update up to ApplyComplete: the image is staged, ActivateFirmware not sent */
static void ua_update_to_staged(void)
{
	ua_request_update();
	zassert_equal(ua_pass_component(COMP_ID), PLDM_CRC_COMP_CAN_BE_UPDATED);
	ua_update_component(sizeof(img));
	zassert_equal(ua_serve_image(img, sizeof(img), -1), PLDM_FWUP_TRANSFER_SUCCESS);
	zassert_equal(ua_answer_complete(PLDM_VERIFY_COMPLETE), PLDM_FWUP_VERIFY_SUCCESS);
	zassert_equal(ua_answer_complete(PLDM_APPLY_COMPLETE), PLDM_FWUP_APPLY_SUCCESS);
}

static void *suite_setup(void)
{
	zassert_ok(tt_pldm_fw_update_init());
	slot1_img_offset = boot_get_image_start_offset(SLOT1);
	return NULL;
}

static void before_each(void *fixture)
{
	const struct flash_area *fa;
	uint8_t junk[64];
	size_t len;

	ARG_UNUSED(fixture);

	/* Leave update mode if a failed test left the firmware device in it */
	zassert_ok(encode_cancel_update_req(next_iid(), req, PLDM_CANCEL_UPDATE_REQ_BYTES));
	len = ua_request(PLDM_CANCEL_UPDATE_REQ_BYTES);
	ARG_UNUSED(len);

	/* Running image A in slot0 */
	make_image(img, 0, 33, 110);
	fa = open_area(SLOT0);
	zassert_ok(flash_area_erase(fa, 0, fa->fa_size));
	zassert_ok(flash_area_write(fa, 0, img, sizeof(img)));

	/* slot1 dirty where the new image goes, with a stale pending upgrade */
	memset(junk, 0x5a, sizeof(junk));
	fa = open_area(SLOT1);
	zassert_ok(flash_area_erase(fa, 0, fa->fa_size));
	zassert_ok(flash_area_write(fa, slot1_img_offset, junk, sizeof(junk)));
	zassert_ok(flash_area_write(fa, slot1_img_offset + 4100U, junk, sizeof(junk)));
	zassert_ok(boot_request_upgrade(BOOT_UPGRADE_TEST));
	zassert_equal(mcuboot_swap_type(), BOOT_SWAP_TYPE_TEST, "stale pending not set up");

	/* The image the UA sends: B */
	make_image(img, 0, 33, 111);
}

ZTEST(pldm_fw_update, test_full_update)
{
	static uint8_t readback[IMG_TOTAL_SIZE];
	char active[PLDM_FIRMWARE_MAX_STRING + 1];
	char pending[PLDM_FIRMWARE_MAX_STRING + 1];

	get_fw_params(active, pending);
	zassert_str_equal(active, "0.33.110+0");

	ua_request_update();
	zassert_equal(ua_pass_component(COMP_ID), PLDM_CRC_COMP_CAN_BE_UPDATED);
	ua_update_component(sizeof(img));

	zassert_equal(ua_serve_image(img, sizeof(img), -1), PLDM_FWUP_TRANSFER_SUCCESS);
	zassert_equal(ua_answer_complete(PLDM_VERIFY_COMPLETE), PLDM_FWUP_VERIFY_SUCCESS);
	zassert_equal(ua_answer_complete(PLDM_APPLY_COMPLETE), PLDM_FWUP_APPLY_SUCCESS);

	/* The stale pending upgrade is gone, and nothing is pending before ActivateFirmware */
	zassert_equal(mcuboot_swap_type(), BOOT_SWAP_TYPE_NONE);

	zassert_ok(flash_area_read(open_area(SLOT1), slot1_img_offset, readback, sizeof(readback)));
	zassert_mem_equal(readback, img, sizeof(img));

	zassert_equal(ua_activate(), PLDM_SUCCESS);
	zassert_equal(mcuboot_swap_type(), BOOT_SWAP_TYPE_TEST);

	get_fw_params(active, pending);
	zassert_str_equal(active, "0.33.110+0");
	zassert_str_equal(pending, "0.33.111+0");
}

ZTEST(pldm_fw_update, test_bad_header_fails_verify)
{
	/* Not an MCUboot image */
	img[0] ^= 0xffU;

	ua_request_update();
	zassert_equal(ua_pass_component(COMP_ID), PLDM_CRC_COMP_CAN_BE_UPDATED);
	ua_update_component(sizeof(img));

	zassert_equal(ua_serve_image(img, sizeof(img), -1), PLDM_FWUP_TRANSFER_SUCCESS);
	zassert_equal(ua_answer_complete(PLDM_VERIFY_COMPLETE),
		      PLDM_FWUP_VERIFY_ERROR_VERIFICATION_FAILURE);

	ua_cancel_update();
	zassert_equal(mcuboot_swap_type(), BOOT_SWAP_TYPE_NONE);
}

ZTEST(pldm_fw_update, test_wrong_component_refused)
{
	ua_request_update();
	zassert_equal(ua_pass_component(COMP_ID + 1), PLDM_CRC_COMP_NOT_SUPPORTED);
	ua_cancel_update();

	/* Nothing was written, so the stale pending upgrade is untouched */
	zassert_equal(mcuboot_swap_type(), BOOT_SWAP_TYPE_TEST);
}

ZTEST(pldm_fw_update, test_cancel_mid_transfer)
{
	char active[PLDM_FIRMWARE_MAX_STRING + 1];
	char pending[PLDM_FIRMWARE_MAX_STRING + 1];

	ua_request_update();
	zassert_equal(ua_pass_component(COMP_ID), PLDM_CRC_COMP_CAN_BE_UPDATED);
	ua_update_component(sizeof(img));

	/* Enough to flush past the first 512-byte write buffer */
	zassert_equal(ua_serve_image(img, sizeof(img), 40), 0xff);
	ua_cancel_update();

	zassert_not_equal(ua_activate(), PLDM_SUCCESS);
	zassert_equal(mcuboot_swap_type(), BOOT_SWAP_TYPE_NONE);

	get_fw_params(active, pending);
	zassert_str_equal(active, "0.33.110+0");
	zassert_str_equal(pending, "");
}

ZTEST(pldm_fw_update, test_device_identifiers_from_devicetree)
{
	uint32_t ids_len;
	uint8_t cc, count;
	uint8_t *data;
	uint16_t type;
	struct variable_field value;
	size_t len;

	zassert_ok(encode_query_device_identifiers_req(
		next_iid(), PLDM_QUERY_DEVICE_IDENTIFIERS_REQ_BYTES, req));
	len = ua_request(PLDM_QUERY_DEVICE_IDENTIFIERS_REQ_BYTES);
	zassert_ok(decode_query_device_identifiers_resp(resp, len, &cc, &ids_len, &count, &data));
	zassert_equal(cc, PLDM_SUCCESS);
	zassert_equal(count, 1U);
	zassert_ok(decode_descriptor_type_length_value(data, ids_len, &type, &value));
	zassert_equal(type, PLDM_FWUP_UUID);
	zassert_equal(value.length, sizeof(expected_uuid));
	zassert_mem_equal(value.ptr, expected_uuid, sizeof(expected_uuid));
}

ZTEST(pldm_fw_update, test_refused_while_running_image_unconfirmed)
{
	uint8_t revert_img[64], readback[64];
	const struct flash_area *fa;

	/* slot1 holds the image MCUboot would revert to; no upgrade is pending */
	memset(revert_img, 0xa5, sizeof(revert_img));
	fa = open_area(SLOT1);
	zassert_ok(flash_area_erase(fa, 0, fa->fa_size));
	zassert_ok(flash_area_write(fa, slot1_img_offset, revert_img, sizeof(revert_img)));

	/* slot0 runs an image a test swap put there, not confirmed yet */
	fa = open_area(SLOT0);
	zassert_ok(boot_write_magic(fa));
	zassert_ok(boot_write_copy_done(fa));
	zassert_equal(mcuboot_swap_type(), BOOT_SWAP_TYPE_REVERT, "REVERT state not set up");

	ua_request_update();
	zassert_equal(ua_pass_component(COMP_ID), PLDM_CRC_COMP_PREREQUISITES_NOT_MET);
	zassert_not_equal(ua_update_component_code(sizeof(img)), PLDM_CCRC_NO_RESPONSE_CODE);
	ua_cancel_update();

	/* The revert image is intact and MCUboot still reverts to it */
	zassert_ok(flash_area_read(open_area(SLOT1), slot1_img_offset, readback, sizeof(readback)));
	zassert_mem_equal(readback, revert_img, sizeof(revert_img));
	zassert_equal(mcuboot_swap_type(), BOOT_SWAP_TYPE_REVERT);
}

ZTEST(pldm_fw_update, test_cancelled_update_not_activated_later)
{
	ua_update_to_staged();

	/* The update agent gives up before ActivateFirmware */
	ua_cancel_update();

	/* A later update that goes straight to ActivateFirmware must not swap that image in */
	ua_request_update();
	zassert_equal(ua_pass_component(COMP_ID), PLDM_CRC_COMP_CAN_BE_UPDATED);
	zassert_not_equal(ua_activate(), PLDM_SUCCESS);
	zassert_equal(mcuboot_swap_type(), BOOT_SWAP_TYPE_NONE);
	ua_cancel_update();
}

ZTEST(pldm_fw_update, test_cancelled_update_not_activated_by_unknown_component)
{
	ua_update_to_staged();
	ua_cancel_update();

	/* libpldm never calls the device for a component it does not list */
	ua_request_update();
	zassert_equal(ua_pass_component(COMP_ID + 1), PLDM_CRC_COMP_NOT_SUPPORTED);
	zassert_not_equal(ua_activate(), PLDM_SUCCESS, "stale staged image activated");
	zassert_equal(mcuboot_swap_type(), BOOT_SWAP_TYPE_NONE);
	ua_cancel_update();
}

ZTEST(pldm_fw_update, test_oversize_image_refused)
{
	ua_request_update();
	zassert_equal(ua_pass_component(COMP_ID), PLDM_CRC_COMP_CAN_BE_UPDATED);
	/* The whole slot: no room left for MCUboot's trailer */
	zassert_not_equal(ua_update_component_code(PARTITION_SIZE(slot0_partition)),
			  PLDM_CCRC_NO_RESPONSE_CODE);
	ua_cancel_update();
}

ZTEST(pldm_fw_update, test_self_contained_activation_refused)
{
	ua_update_to_staged();
	zassert_equal(ua_activate_with(true), PLDM_FWUP_SELF_CONTAINED_ACTIVATION_NOT_PERMITTED);
	zassert_equal(mcuboot_swap_type(), BOOT_SWAP_TYPE_NONE);
	ua_cancel_update();
}

ZTEST_SUITE(pldm_fw_update, NULL, suite_setup, before_each, NULL, NULL);
