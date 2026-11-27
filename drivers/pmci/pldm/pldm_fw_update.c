/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DSP0267 firmware device on top of libpldm's responder (pldm_fd_*).
 *
 * One component, the application image, with the identity given by the
 * tenstorrent,pldm-fw-update devicetree node. The update agent's image is written
 * to the MCUboot secondary slot. ActivateFirmware requests a test swap and nothing
 * else: the new image runs after the next reset, and confirms itself from main().
 * While the running image is unconfirmed, the secondary slot holds the image
 * MCUboot would revert to, so no update is accepted until it confirms.
 */

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <libpldm/firmware_fd.h>
#include <libpldm/firmware_update.h>

/*
 * libpldm's Zephyr build does not generate libpldm/sizes.h (PLDM_SIZEOF_PLDM_FD),
 * so size the responder's storage from its own definition.
 */
#include <firmware_device/fd-internal.h>

#include <errno.h>
#include <string.h>

#include "pldm_fw_update.h"

LOG_MODULE_REGISTER(tt_pldm_fw_update, CONFIG_TT_APP_LOG_LEVEL);

#define FWU_ACTIVE_SLOT PARTITION_ID(slot0_partition)
#define FWU_UPDATE_SLOT PARTITION_ID(slot1_partition)
/* The last erase block of a slot holds MCUboot's trailer */
#define FWU_MAX_IMG_SIZE                                                                           \
	(PARTITION_SIZE(slot0_partition) -                                                         \
	 DT_PROP(DT_GPARENT(DT_NODELABEL(slot0_partition)), erase_block_size))

#define FWU_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(tenstorrent_pldm_fw_update)

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(tenstorrent_pldm_fw_update) == 1,
	     "Exactly one tenstorrent,pldm-fw-update node is supported");
BUILD_ASSERT(sizeof(DT_PROP(FWU_NODE, uuid)) == 37,
	     "tenstorrent,pldm-fw-update uuid must be xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx");
BUILD_ASSERT(DT_PROP(FWU_NODE, component_id) <= UINT16_MAX,
	     "tenstorrent,pldm-fw-update component-id must fit 16 bits");

#define FWU_COMP_CLASSIFICATION PLDM_COMP_FIRMWARE
#define FWU_COMP_IDENTIFIER     DT_PROP(FWU_NODE, component_id)

const ver32_t tt_pldm_fw_update_versions[2] = {
	{.alpha = 0x00, .update = 0xf0, .minor = 0xf1, .major = 0xf1},
	{.alpha = 0xba, .update = 0xbe, .minor = 0x9d, .major = 0x53},
};

const bitfield8_t tt_pldm_fw_update_commands[32] = {
	[0] = {.byte = BIT(PLDM_QUERY_DEVICE_IDENTIFIERS) | BIT(PLDM_GET_FIRMWARE_PARAMETERS)},
	[2] = {.byte = (BIT(PLDM_REQUEST_UPDATE) | BIT(PLDM_PASS_COMPONENT_TABLE) |
			BIT(PLDM_UPDATE_COMPONENT)) >>
		       16},
	[3] = {.byte = (BIT(PLDM_ACTIVATE_FIRMWARE) | BIT(PLDM_GET_STATUS) |
			BIT(PLDM_CANCEL_UPDATE_COMPONENT) | BIT(PLDM_CANCEL_UPDATE)) >>
		       24},
};

struct fwu_state {
	bool initialised;
	/* An UpdateComponent was accepted and data is going to the update slot */
	bool writing;
	/* The update slot holds a verified image that ActivateFirmware may request */
	bool staged;
	uint32_t next_offset;
	struct flash_img_context img;
	uint8_t uuid[PLDM_FWUP_UUID_LENGTH];
	struct pldm_descriptor descriptor;
	struct pldm_firmware_component_standalone comp;
	const struct pldm_firmware_component_standalone *comps[1];
};

static struct fwu_state fwu;
static struct pldm_fd fwu_fd;

static void fwu_version_none(struct pldm_firmware_version *ver)
{
	memset(ver, 0, sizeof(*ver));
	/* An empty ASCII string fails to decode at the update agent; say UNKNOWN instead */
	ver->str.str_type = PLDM_STR_TYPE_UNKNOWN;
}

static void fwu_version_from_slot(uint8_t area_id, struct pldm_firmware_version *ver)
{
	struct mcuboot_img_header hdr;
	const struct mcuboot_img_sem_ver *v;
	int len;

	fwu_version_none(ver);

	if (boot_read_bank_header(area_id, &hdr, sizeof(hdr)) != 0) {
		return;
	}

	v = &hdr.h.v1.sem_ver;
	len = snprintk((char *)ver->str.str_data, sizeof(ver->str.str_data), "%u.%u.%u+%u",
		       v->major, v->minor, v->revision, v->build_num);
	if (len <= 0 || (size_t)len >= sizeof(ver->str.str_data)) {
		return;
	}

	ver->str.str_type = PLDM_STR_TYPE_ASCII;
	ver->str.str_len = len;
	ver->comparison_stamp =
		((uint32_t)v->major << 24) | ((uint32_t)v->minor << 16) | v->revision;
}

static void fwu_refresh_versions(void)
{
	int swap_type = mcuboot_swap_type();

	fwu_version_from_slot(FWU_ACTIVE_SLOT, &fwu.comp.active_ver);

	if (swap_type == BOOT_SWAP_TYPE_TEST || swap_type == BOOT_SWAP_TYPE_PERM) {
		fwu_version_from_slot(FWU_UPDATE_SLOT, &fwu.comp.pending_ver);
	} else {
		fwu_version_none(&fwu.comp.pending_ver);
	}
}

static int fwu_device_identifiers(void *ctx, uint8_t *ret_count,
				  const struct pldm_descriptor **ret_descriptors)
{
	ARG_UNUSED(ctx);

	*ret_count = 1;
	*ret_descriptors = &fwu.descriptor;
	return 0;
}

static int fwu_components(void *ctx, uint16_t *ret_count,
			  const struct pldm_firmware_component_standalone ***ret_entries)
{
	ARG_UNUSED(ctx);

	fwu_refresh_versions();
	*ret_count = ARRAY_SIZE(fwu.comps);
	*ret_entries = fwu.comps;
	return 0;
}

static int fwu_imageset_versions(void *ctx, struct pldm_firmware_string *ret_active,
				 struct pldm_firmware_string *ret_pending)
{
	ARG_UNUSED(ctx);

	fwu_refresh_versions();
	*ret_active = fwu.comp.active_ver.str;
	*ret_pending = fwu.comp.pending_ver.str;
	return 0;
}

static void fwu_abandon(void)
{
	fwu.writing = false;
	fwu.staged = false;
	fwu.next_offset = 0;
}

static enum pldm_component_response_codes
fwu_update_component(void *ctx, bool update, const struct pldm_firmware_update_component *comp)
{
	int swap_type;
	int rc;

	ARG_UNUSED(ctx);

	/* libpldm only calls this for a component the device lists */
	swap_type = mcuboot_swap_type();

	/*
	 * REVERT: the running image has not confirmed itself, so the secondary slot holds the
	 * image MCUboot reverts to if it never does, and writing the slot would destroy it.
	 * Anything but a known state (e.g. a trailer read error): refuse too. TEST and PERM
	 * are an earlier update still pending, which a new image replaces.
	 */
	if (swap_type != BOOT_SWAP_TYPE_NONE && swap_type != BOOT_SWAP_TYPE_TEST &&
	    swap_type != BOOT_SWAP_TYPE_PERM) {
		LOG_WRN("No update in MCUboot swap state %d (unconfirmed image?)", swap_type);
		return PLDM_CRC_COMP_PREREQUISITES_NOT_MET;
	}

	if (!update) {
		/* PassComponentTable: no size yet */
		return PLDM_CRC_COMP_CAN_BE_UPDATED;
	}

	if (comp->comp_image_size == 0 || comp->comp_image_size > FWU_MAX_IMG_SIZE) {
		LOG_WRN("Image size %u does not fit the slot (%u)", comp->comp_image_size,
			(unsigned int)FWU_MAX_IMG_SIZE);
		return PLDM_CRC_COMP_PREREQUISITES_NOT_MET;
	}

	fwu_abandon();

	rc = flash_img_init_id(&fwu.img, FWU_UPDATE_SLOT);
	if (rc != 0) {
		LOG_ERR("%s() failed: %d", "flash_img_init_id", rc);
		return PLDM_CRC_COMP_PREREQUISITES_NOT_MET;
	}

	fwu.writing = true;
	LOG_INF("Update started: %u bytes", comp->comp_image_size);
	return PLDM_CRC_COMP_CAN_BE_UPDATED;
}

static uint32_t fwu_transfer_size(void *ctx, uint32_t ua_max_transfer_size)
{
	ARG_UNUSED(ctx);

	/*
	 * libpldm calls this on every RequestUpdate, the start of a new update: an image
	 * staged by an earlier one that was cancelled or timed out before ActivateFirmware
	 * must not be activated by this one.
	 */
	fwu.staged = false;

	return MIN(ua_max_transfer_size, CONFIG_TT_PMCI_PLDM_FW_UPDATE_TRANSFER_SIZE);
}

static uint8_t fwu_firmware_data(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len,
				 const struct pldm_firmware_update_component *comp)
{
	bool last;
	int rc;

	ARG_UNUSED(ctx);

	/* libpldm requests the image in order, so anything else is a bug */
	if (!fwu.writing || offset != fwu.next_offset || len > comp->comp_image_size - offset) {
		LOG_ERR("Unexpected data at %u (+%u), expected %u", offset, len, fwu.next_offset);
		return PLDM_FWUP_FD_ABORTED_TRANSFER;
	}

	last = (offset + len == comp->comp_image_size);
	rc = flash_img_buffered_write(&fwu.img, data, len, last);
	if (rc != 0) {
		LOG_ERR("%s() failed: %d", "flash_img_buffered_write", rc);
		return PLDM_FWUP_FD_ABORTED_TRANSFER_STORAGE_ISSUE;
	}

	fwu.next_offset += len;
	return PLDM_FWUP_TRANSFER_SUCCESS;
}

static uint8_t fwu_verify(void *ctx, const struct pldm_firmware_update_component *comp,
			  bool *ret_pending, uint8_t *ret_progress_percent)
{
	struct mcuboot_img_header hdr;
	int rc;

	ARG_UNUSED(ctx);
	ARG_UNUSED(ret_pending);
	ARG_UNUSED(ret_progress_percent);

	if (!fwu.writing || fwu.next_offset != comp->comp_image_size) {
		return PLDM_FWUP_VERIFY_ERROR_IMAGE_INCOMPLETE;
	}

	/*
	 * Only check that this is an MCUboot image that fits what was sent. MCUboot
	 * checks the signature before it swaps, and refuses the image if it fails.
	 */
	rc = boot_read_bank_header(FWU_UPDATE_SLOT, &hdr, sizeof(hdr));
	if (rc != 0) {
		LOG_ERR("No MCUboot image header in the update slot: %d", rc);
		return PLDM_FWUP_VERIFY_ERROR_VERIFICATION_FAILURE;
	}

	if (hdr.h.v1.image_size >= comp->comp_image_size) {
		LOG_ERR("Header image size %u does not fit the %u bytes sent", hdr.h.v1.image_size,
			comp->comp_image_size);
		return PLDM_FWUP_VERIFY_ERROR_VERIFICATION_FAILURE;
	}

	return PLDM_FWUP_VERIFY_SUCCESS;
}

static uint8_t fwu_apply(void *ctx, const struct pldm_firmware_update_component *comp,
			 bool *ret_pending, uint8_t *ret_progress_percent)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(comp);
	ARG_UNUSED(ret_pending);
	ARG_UNUSED(ret_progress_percent);

	/* Nothing to apply: the image is already in place for MCUboot */
	fwu.writing = false;
	fwu.staged = true;
	return PLDM_FWUP_APPLY_SUCCESS;
}

static uint8_t fwu_activate(void *ctx, bool self_contained, uint16_t *ret_estimated_time)
{
	int rc;

	ARG_UNUSED(ctx);
	ARG_UNUSED(ret_estimated_time);

	if (!fwu.staged) {
		return PLDM_FWUP_INCOMPLETE_UPDATE;
	}

	if (self_contained) {
		/* The component's activation methods do not include self-contained */
		LOG_WRN("Self-contained activation is not supported");
		return PLDM_FWUP_SELF_CONTAINED_ACTIVATION_NOT_PERMITTED;
	}

	/* Test swap: if the new image never confirms itself, MCUboot reverts it */
	rc = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (rc != 0) {
		LOG_ERR("%s() failed: %d", "boot_request_upgrade", rc);
		return PLDM_ERROR;
	}

	fwu.staged = false;
	LOG_INF("Update pending; it runs after the next reset");
	return PLDM_SUCCESS;
}

static void fwu_cancel_update_component(void *ctx,
					const struct pldm_firmware_update_component *comp)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(comp);

	LOG_INF("Update cancelled");
	fwu_abandon();
}

static uint64_t fwu_now(void *ctx)
{
	ARG_UNUSED(ctx);

	return k_uptime_get();
}

static const struct pldm_fd_ops fwu_ops = {
	.device_identifiers = fwu_device_identifiers,
	.components = fwu_components,
	.imageset_versions = fwu_imageset_versions,
	.update_component = fwu_update_component,
	.transfer_size = fwu_transfer_size,
	.firmware_data = fwu_firmware_data,
	.verify = fwu_verify,
	.apply = fwu_apply,
	.activate = fwu_activate,
	.cancel_update_component = fwu_cancel_update_component,
	.now = fwu_now,
};

static int fwu_parse_uuid(const char *str, uint8_t uuid[PLDM_FWUP_UUID_LENGTH])
{
	char hex[2 * PLDM_FWUP_UUID_LENGTH];
	size_t n = 0;

	for (; *str != '\0'; str++) {
		if (*str == '-') {
			continue;
		}
		if (n == sizeof(hex)) {
			return -EINVAL;
		}
		hex[n++] = *str;
	}

	if (n != sizeof(hex) ||
	    hex2bin(hex, sizeof(hex), uuid, PLDM_FWUP_UUID_LENGTH) != PLDM_FWUP_UUID_LENGTH) {
		return -EINVAL;
	}

	return 0;
}

int tt_pldm_fw_update_init(void)
{
	int rc;

	if (fwu.initialised) {
		return 0;
	}

	rc = fwu_parse_uuid(DT_PROP(FWU_NODE, uuid), fwu.uuid);
	if (rc != 0) {
		LOG_ERR("Bad tenstorrent,pldm-fw-update uuid: %s", DT_PROP(FWU_NODE, uuid));
		return rc;
	}

	fwu.descriptor = (struct pldm_descriptor){
		.descriptor_type = PLDM_FWUP_UUID,
		.descriptor_length = PLDM_FWUP_UUID_LENGTH,
		.descriptor_data = fwu.uuid,
	};

	fwu.comp = (struct pldm_firmware_component_standalone){
		.comp_classification = FWU_COMP_CLASSIFICATION,
		.comp_identifier = FWU_COMP_IDENTIFIER,
		.comp_classification_index = 0,
		/* Every device on the board is power-cycled together; none resets alone */
		.comp_activation_methods = {.value = BIT(PLDM_ACTIVATION_DC_POWER_CYCLE) |
						     BIT(PLDM_ACTIVATION_AC_POWER_CYCLE)},
	};
	fwu.comps[0] = &fwu.comp;

	rc = pldm_fd_setup(&fwu_fd, sizeof(fwu_fd), &fwu_ops, NULL, NULL);
	if (rc != 0) {
		LOG_ERR("%s() failed: %d", "pldm_fd_setup", rc);
		return rc;
	}

	fwu.initialised = true;
	return 0;
}

int tt_pldm_fw_update_handle_msg(uint8_t eid, const void *msg, size_t len, void *resp,
				 size_t *resp_len)
{
	if (!fwu.initialised) {
		return -ENODEV;
	}

	return pldm_fd_handle_msg(&fwu_fd, eid, msg, len, resp, resp_len);
}

int tt_pldm_fw_update_progress(void *req, size_t *req_len, uint8_t *eid)
{
	pldm_tid_t address = 0;
	int rc;

	if (!fwu.initialised) {
		*req_len = 0;
		return -ENODEV;
	}

	rc = pldm_fd_progress(&fwu_fd, req, req_len, &address);
	*eid = address;
	return rc;
}
