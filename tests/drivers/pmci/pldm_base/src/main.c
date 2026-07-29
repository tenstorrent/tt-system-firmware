/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <libpldm/base.h>

#include <stddef.h>
#include <stdint.h>

#include "pldm_base.h"

#define RESP_BUF_PAYLOAD_MAX 64U

static struct pldm_header_info base_req_hdr(uint8_t instance, uint8_t command)
{
	return (struct pldm_header_info){
		.msg_type = PLDM_REQUEST,
		.instance = instance,
		.pldm_type = PLDM_BASE,
		.command = command,
	};
}

static void assert_response_header(const struct pldm_msg *resp_msg, uint8_t instance,
				   uint8_t command)
{
	struct pldm_header_info hdr;
	int rc;

	rc = unpack_pldm_header(&resp_msg->hdr, &hdr);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(hdr.msg_type, PLDM_RESPONSE);
	zassert_equal(hdr.instance, instance);
	zassert_equal(hdr.pldm_type, PLDM_BASE);
	zassert_equal(hdr.command, command);
}

ZTEST(pldm_base, test_get_tid_success)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_TID_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 7U;
	struct pldm_header_info req_hdr = base_req_hdr(instance, PLDM_GET_TID);
	uint8_t tid = 0x2aU;
	uint8_t cc;
	uint8_t resp_tid;
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_tid_req(instance, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_base_build_response(&tid, NULL, &req_hdr, req_msg, PLDM_GET_TID_REQ_BYTES,
				      resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + PLDM_GET_TID_RESP_BYTES);

	assert_response_header(resp_msg, instance, PLDM_GET_TID);

	rc = decode_get_tid_resp(resp_msg, resp_pldm_len - sizeof(struct pldm_msg_hdr), &cc,
				 &resp_tid);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(cc, PLDM_SUCCESS);
	zassert_equal(resp_tid, tid);
}

ZTEST(pldm_base, test_set_tid_success)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_SET_TID_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 8U;
	const uint8_t new_tid = 0x3cU;
	struct pldm_header_info req_hdr = base_req_hdr(instance, PLDM_SET_TID);
	uint8_t tid = 1U;
	size_t resp_pldm_len;
	int rc;

	rc = encode_set_tid_req(instance, new_tid, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_base_build_response(&tid, NULL, &req_hdr, req_msg, PLDM_SET_TID_REQ_BYTES,
				      resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(tid, new_tid);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + 1U);

	assert_response_header(resp_msg, instance, PLDM_SET_TID);
	zassert_equal(resp_msg->payload[0], PLDM_SUCCESS);
}

ZTEST(pldm_base, test_set_tid_rejects_reserved_tid)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_SET_TID_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 9U;
	struct pldm_header_info req_hdr = base_req_hdr(instance, PLDM_SET_TID);
	uint8_t tid = 0x21U;
	size_t resp_pldm_len;
	int rc;

	rc = encode_pldm_header_only(PLDM_REQUEST, instance, PLDM_BASE, PLDM_SET_TID, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);
	req_msg->payload[0] = 0x00U;

	rc = pldm_base_build_response(&tid, NULL, &req_hdr, req_msg, PLDM_SET_TID_REQ_BYTES,
				      resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(tid, 0x21U);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + 1U);

	assert_response_header(resp_msg, instance, PLDM_SET_TID);
	zassert_equal(resp_msg->payload[0], PLDM_ERROR_INVALID_DATA);
}

ZTEST(pldm_base, test_set_tid_rejects_invalid_length)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_SET_TID_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 10U;
	struct pldm_header_info req_hdr = base_req_hdr(instance, PLDM_SET_TID);
	uint8_t tid = 0x21U;
	size_t resp_pldm_len;
	int rc;

	rc = encode_set_tid_req(instance, 0x3cU, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_base_build_response(&tid, NULL, &req_hdr, req_msg, 0U, resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(tid, 0x21U);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + 1U);

	assert_response_header(resp_msg, instance, PLDM_SET_TID);
	zassert_equal(resp_msg->payload[0], PLDM_ERROR_INVALID_LENGTH);
}

ZTEST(pldm_base, test_get_commands_base)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_COMMANDS_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 10U;
	const ver32_t version = {0};
	struct pldm_header_info req_hdr = base_req_hdr(instance, PLDM_GET_PLDM_COMMANDS);
	bitfield8_t commands[PLDM_MAX_CMDS_PER_TYPE / 8] = {0};
	uint8_t cc;
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_commands_req(instance, PLDM_BASE, version, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_base_build_response(&cc, NULL, &req_hdr, req_msg, PLDM_GET_COMMANDS_REQ_BYTES,
				      resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + PLDM_GET_COMMANDS_RESP_BYTES);

	assert_response_header(resp_msg, instance, PLDM_GET_PLDM_COMMANDS);

	rc = decode_get_commands_resp(resp_msg, resp_pldm_len - sizeof(struct pldm_msg_hdr), &cc,
				      commands);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(cc, PLDM_SUCCESS);
	zassert_true((commands[0].byte & BIT(PLDM_GET_TID)) != 0U);
	zassert_true((commands[0].byte & BIT(PLDM_SET_TID)) != 0U);
	zassert_true((commands[0].byte & BIT(PLDM_GET_PLDM_VERSION)) != 0U);
	zassert_true((commands[0].byte & BIT(PLDM_GET_PLDM_TYPES)) != 0U);
	zassert_true((commands[0].byte & BIT(PLDM_GET_PLDM_COMMANDS)) != 0U);
}

ZTEST(pldm_base, test_get_commands_rejects_invalid_type)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_COMMANDS_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 14U;
	const ver32_t version = {0};
	struct pldm_header_info req_hdr = base_req_hdr(instance, PLDM_GET_PLDM_COMMANDS);
	uint8_t tid = 0x2aU;
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_commands_req(instance, PLDM_SMBIOS, version, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_base_build_response(&tid, NULL, &req_hdr, req_msg, PLDM_GET_COMMANDS_REQ_BYTES,
				      resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + 1U);

	assert_response_header(resp_msg, instance, PLDM_GET_PLDM_COMMANDS);
	zassert_equal(resp_msg->payload[0],
		      PLDM_GET_PLDM_COMMANDS_INVALID_PLDM_TYPE_IN_REQUEST_DATA);
}

ZTEST(pldm_base, test_get_types_base_success)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_TYPES_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 11U;
	struct pldm_header_info req_hdr = base_req_hdr(instance, PLDM_GET_PLDM_TYPES);
	struct pldm_base_get_pldm_types_resp types_resp = {0};
	uint8_t tid = 0x2aU;
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_types_req(instance, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_base_build_response(&tid, NULL, &req_hdr, req_msg, PLDM_GET_TYPES_REQ_BYTES,
				      resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(resp_pldm_len,
		      sizeof(struct pldm_msg_hdr) + PLDM_BASE_GET_PLDM_TYPES_RESP_BYTES);

	assert_response_header(resp_msg, instance, PLDM_GET_PLDM_TYPES);

	rc = decode_pldm_base_get_pldm_types_resp(
		resp_msg, resp_pldm_len - sizeof(struct pldm_msg_hdr), &types_resp);
	zassert_equal(rc, 0);
	zassert_equal(types_resp.completion_code, PLDM_SUCCESS);
	zassert_true((types_resp.pldm_types[PLDM_BASE / 8].byte & BIT(PLDM_BASE % 8)) != 0U);
}

ZTEST(pldm_base, test_get_types_rejects_invalid_length)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_TYPES_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 12U;
	struct pldm_header_info req_hdr = base_req_hdr(instance, PLDM_GET_PLDM_TYPES);
	uint8_t tid = 0x2aU;
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_types_req(instance, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_base_build_response(&tid, NULL, &req_hdr, req_msg, 1U, resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + 1U);

	assert_response_header(resp_msg, instance, PLDM_GET_PLDM_TYPES);
	zassert_equal(resp_msg->payload[0], PLDM_ERROR_INVALID_LENGTH);
}

ZTEST(pldm_base, test_unsupported_command_returns_error)
{
	PLDM_MSG_BUFFER(req_buf, 0U);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 13U;
	const uint8_t unsupported_cmd = 0xffU;
	struct pldm_header_info req_hdr = base_req_hdr(instance, unsupported_cmd);
	uint8_t tid = 0x2aU;
	size_t resp_pldm_len;
	int rc;

	rc = encode_pldm_header_only(PLDM_REQUEST, instance, PLDM_BASE, unsupported_cmd, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_base_build_response(&tid, NULL, &req_hdr, req_msg, 0U, resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + 1U);

	assert_response_header(resp_msg, instance, unsupported_cmd);
	zassert_equal(resp_msg->payload[0], PLDM_ERROR_UNSUPPORTED_PLDM_CMD);
}

ZTEST(pldm_base, test_get_version_rejects_nextpart_flag)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_VERSION_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 11U;
	struct pldm_header_info req_hdr = base_req_hdr(instance, PLDM_GET_PLDM_VERSION);
	uint8_t tid = 0x2aU;
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_version_req(instance, 0U, PLDM_GET_NEXTPART, PLDM_BASE, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_base_build_response(&tid, NULL, &req_hdr, req_msg, PLDM_GET_VERSION_REQ_BYTES,
				      resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + 1U);

	assert_response_header(resp_msg, instance, PLDM_GET_PLDM_VERSION);
	zassert_equal(resp_msg->payload[0], PLDM_GET_PLDM_VERSION_INVALID_TRANSFER_OPERATION_FLAG);
}

ZTEST(pldm_base, test_get_version_base_success)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_VERSION_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 12U;
	struct pldm_header_info req_hdr = base_req_hdr(instance, PLDM_GET_PLDM_VERSION);
	uint8_t tid = 0x2aU;
	const struct pldm_get_version_resp *ver_resp;
	size_t payload_len;
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_version_req(instance, 0U, PLDM_GET_FIRSTPART, PLDM_BASE, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_base_build_response(&tid, NULL, &req_hdr, req_msg, PLDM_GET_VERSION_REQ_BYTES,
				      resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);

	payload_len = resp_pldm_len - sizeof(struct pldm_msg_hdr);
	zassert_equal(payload_len, 14U);

	assert_response_header(resp_msg, instance, PLDM_GET_PLDM_VERSION);

	ver_resp = (const struct pldm_get_version_resp *)resp_msg->payload;
	zassert_equal(ver_resp->completion_code, PLDM_SUCCESS);
	zassert_equal(sys_le32_to_cpu(ver_resp->next_transfer_handle), 0U);
	zassert_equal(ver_resp->transfer_flag, PLDM_START_AND_END);
}

ZTEST_SUITE(pldm_base, NULL, NULL, NULL, NULL, NULL);
