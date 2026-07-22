/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <libpldm/platform.h>

#include <stddef.h>
#include <stdint.h>

#include "pldm_platform.h"

#define RESP_BUF_PAYLOAD_MAX 128U
#define TEST_PDR_NODE        DT_NODELABEL(pldm_pdr_test)

static const struct device *const pdr_dev = DEVICE_DT_GET(TEST_PDR_NODE);

static void *suite_setup(void)
{
	zassert_true(device_is_ready(pdr_dev));
	return NULL;
}

static struct pldm_header_info platform_req_hdr(uint8_t instance, uint8_t command)
{
	return (struct pldm_header_info){
		.msg_type = PLDM_REQUEST,
		.instance = instance,
		.pldm_type = PLDM_PLATFORM,
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
	zassert_equal(hdr.pldm_type, PLDM_PLATFORM);
	zassert_equal(hdr.command, command);
}

ZTEST(pldm_platform, test_versions_get_has_entries)
{
	size_t versions_size = 0U;
	const ver32_t *versions = pldm_platform_versions_get(&versions_size);

	zassert_not_null(versions);
	zassert_true(versions_size >= sizeof(ver32_t));
	zassert_equal(versions_size % sizeof(ver32_t), 0U);
}

ZTEST(pldm_platform, test_commands_get_contains_supported_commands)
{
	const bitfield8_t *commands = pldm_platform_commands_get();

	zassert_not_null(commands);
	zassert_true((commands[PLDM_GET_SENSOR_READING / 8].byte &
		      BIT(PLDM_GET_SENSOR_READING % 8)) != 0U);
	zassert_true((commands[PLDM_GET_PDR_REPOSITORY_INFO / 8].byte &
		      BIT(PLDM_GET_PDR_REPOSITORY_INFO % 8)) != 0U);
	zassert_true((commands[PLDM_GET_PDR / 8].byte & BIT(PLDM_GET_PDR % 8)) != 0U);
}

ZTEST(pldm_platform, test_get_pdr_repository_info_success)
{
	PLDM_MSG_BUFFER(req_buf, 0U);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 1U;
	struct pldm_header_info req_hdr = platform_req_hdr(instance, PLDM_GET_PDR_REPOSITORY_INFO);
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_pdr_repository_info_req(instance, req_msg, 0U);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_platform_build_response(pdr_dev, &req_hdr, req_msg, 0U, resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_true(resp_pldm_len > sizeof(struct pldm_msg_hdr));

	assert_response_header(resp_msg, instance, PLDM_GET_PDR_REPOSITORY_INFO);
	zassert_equal(resp_msg->payload[0], PLDM_SUCCESS);
}

ZTEST(pldm_platform, test_get_pdr_repository_info_rejects_invalid_length)
{
	PLDM_MSG_BUFFER(req_buf, 0U);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 2U;
	struct pldm_header_info req_hdr = platform_req_hdr(instance, PLDM_GET_PDR_REPOSITORY_INFO);
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_pdr_repository_info_req(instance, req_msg, 0U);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_platform_build_response(pdr_dev, &req_hdr, req_msg, 1U, resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + 1U);

	assert_response_header(resp_msg, instance, PLDM_GET_PDR_REPOSITORY_INFO);
	zassert_equal(resp_msg->payload[0], PLDM_ERROR_INVALID_LENGTH);
}

ZTEST(pldm_platform, test_get_pdr_success)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_PDR_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 6U;
	struct pldm_header_info req_hdr = platform_req_hdr(instance, PLDM_GET_PDR);
	uint8_t cc;
	uint32_t next_record_handle;
	uint32_t next_transfer_handle;
	uint8_t transfer_flag;
	uint16_t response_count;
	uint8_t record_data[RESP_BUF_PAYLOAD_MAX] = {0};
	uint8_t transfer_crc;
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_pdr_req(instance, 0U, 0U, PLDM_GET_FIRSTPART, 8U,
				PLDM_PDR_RECORD_CHANGE_NUM, req_msg, PLDM_GET_PDR_REQ_BYTES);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_platform_build_response(pdr_dev, &req_hdr, req_msg, PLDM_GET_PDR_REQ_BYTES,
					  resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_true(resp_pldm_len > sizeof(struct pldm_msg_hdr));

	assert_response_header(resp_msg, instance, PLDM_GET_PDR);

	rc = decode_get_pdr_resp(resp_msg, resp_pldm_len - sizeof(struct pldm_msg_hdr), &cc,
				 &next_record_handle, &next_transfer_handle, &transfer_flag,
				 &response_count, record_data, sizeof(record_data), &transfer_crc);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(cc, PLDM_SUCCESS);
	zassert_true(response_count > 0U);
	zassert_true(response_count <= 8U);
	zassert_true(transfer_flag == PLDM_START || transfer_flag == PLDM_START_AND_END);
	if (transfer_flag == PLDM_START_AND_END) {
		zassert_equal(next_transfer_handle, 0U);
	} else {
		zassert_true(next_transfer_handle > 0U);
	}
	zassert_true(next_record_handle > 0U);
	zassert_equal(record_data[0], 1U);
	(void)transfer_crc;
}

ZTEST(pldm_platform, test_get_sensor_reading_invalid_sensor_id)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_SENSOR_READING_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 3U;
	struct pldm_header_info req_hdr = platform_req_hdr(instance, PLDM_GET_SENSOR_READING);
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_sensor_reading_req(instance, 999U, false, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_platform_build_response(pdr_dev, &req_hdr, req_msg,
					  PLDM_GET_SENSOR_READING_REQ_BYTES, resp_msg,
					  &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + 1U);

	assert_response_header(resp_msg, instance, PLDM_GET_SENSOR_READING);
	zassert_equal(resp_msg->payload[0], PLDM_PLATFORM_INVALID_SENSOR_ID);
}

ZTEST(pldm_platform, test_get_sensor_reading_rejects_invalid_length)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_SENSOR_READING_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 9U;
	struct pldm_header_info req_hdr = platform_req_hdr(instance, PLDM_GET_SENSOR_READING);
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_sensor_reading_req(instance, 101U, false, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_platform_build_response(pdr_dev, &req_hdr, req_msg, 0U, resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + 1U);

	assert_response_header(resp_msg, instance, PLDM_GET_SENSOR_READING);
	zassert_equal(resp_msg->payload[0], PLDM_ERROR_INVALID_LENGTH);
}

ZTEST(pldm_platform, test_get_sensor_reading_rearm_unavailable)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_SENSOR_READING_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 4U;
	struct pldm_header_info req_hdr = platform_req_hdr(instance, PLDM_GET_SENSOR_READING);
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_sensor_reading_req(instance, 101U, true, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_platform_build_response(pdr_dev, &req_hdr, req_msg,
					  PLDM_GET_SENSOR_READING_REQ_BYTES, resp_msg,
					  &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + 1U);

	assert_response_header(resp_msg, instance, PLDM_GET_SENSOR_READING);
	zassert_equal(resp_msg->payload[0], PLDM_PLATFORM_REARM_UNAVAILABLE_IN_PRESENT_STATE);
}

ZTEST(pldm_platform, test_get_sensor_reading_sensor_unavailable)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_SENSOR_READING_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 8U;
	struct pldm_header_info req_hdr = platform_req_hdr(instance, PLDM_GET_SENSOR_READING);
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_sensor_reading_req(instance, 404U, false, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_platform_build_response(pdr_dev, &req_hdr, req_msg,
					  PLDM_GET_SENSOR_READING_REQ_BYTES, resp_msg,
					  &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + 1U);

	assert_response_header(resp_msg, instance, PLDM_GET_SENSOR_READING);
	zassert_equal(resp_msg->payload[0], PLDM_SENSOR_UNAVAILABLE);
}

ZTEST(pldm_platform, test_get_sensor_reading_decode_failure_returns_error)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_SENSOR_READING_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 10U;
	struct pldm_header_info req_hdr = platform_req_hdr(instance, PLDM_GET_SENSOR_READING);
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_sensor_reading_req(instance, 505U, false, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_platform_build_response(pdr_dev, &req_hdr, req_msg,
					  PLDM_GET_SENSOR_READING_REQ_BYTES, resp_msg,
					  &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + 1U);

	assert_response_header(resp_msg, instance, PLDM_GET_SENSOR_READING);
	zassert_equal(resp_msg->payload[0], PLDM_ERROR);
}

ZTEST(pldm_platform, test_get_sensor_reading_success)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_SENSOR_READING_REQ_BYTES);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 7U;
	struct pldm_header_info req_hdr = platform_req_hdr(instance, PLDM_GET_SENSOR_READING);
	uint8_t cc;
	uint8_t sensor_data_size;
	uint8_t sensor_operational_state;
	uint8_t sensor_event_message_enable;
	uint8_t present_state;
	uint8_t previous_state;
	uint8_t event_state;
	uint8_t present_reading[sizeof(int32_t)] = {0};
	size_t resp_pldm_len;
	int rc;

	rc = encode_get_sensor_reading_req(instance, 101U, false, req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_platform_build_response(pdr_dev, &req_hdr, req_msg,
					  PLDM_GET_SENSOR_READING_REQ_BYTES, resp_msg,
					  &resp_pldm_len);
	zassert_equal(rc, 0);

	assert_response_header(resp_msg, instance, PLDM_GET_SENSOR_READING);

	rc = decode_get_sensor_reading_resp(resp_msg, resp_pldm_len - sizeof(struct pldm_msg_hdr),
					    &cc, &sensor_data_size, &sensor_operational_state,
					    &sensor_event_message_enable, &present_state,
					    &previous_state, &event_state, present_reading);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(cc, PLDM_SUCCESS);
	zassert_equal(sensor_data_size, PLDM_SENSOR_DATA_SIZE_SINT32);
	zassert_equal(sensor_operational_state, PLDM_SENSOR_ENABLED);
	zassert_equal(sensor_event_message_enable, PLDM_EVENTS_DISABLED);
	zassert_equal(present_state, PLDM_SENSOR_NORMAL);
	ARG_UNUSED(present_reading);
	ARG_UNUSED(previous_state);
	ARG_UNUSED(event_state);
}

ZTEST(pldm_platform, test_unsupported_command_returns_error)
{
	PLDM_MSG_BUFFER(req_buf, 0U);
	PLDM_MSG_BUFFER(resp_buf, RESP_BUF_PAYLOAD_MAX);
	struct pldm_msg *req_msg = (struct pldm_msg *)req_buf;
	struct pldm_msg *resp_msg = (struct pldm_msg *)resp_buf;
	const uint8_t instance = 5U;
	const uint8_t unsupported_cmd = 0xffU;
	struct pldm_header_info req_hdr = platform_req_hdr(instance, unsupported_cmd);
	size_t resp_pldm_len;
	int rc;

	rc = encode_pldm_header_only(PLDM_REQUEST, instance, PLDM_PLATFORM, unsupported_cmd,
				     req_msg);
	zassert_equal(rc, PLDM_SUCCESS);

	rc = pldm_platform_build_response(pdr_dev, &req_hdr, req_msg, 0U, resp_msg, &resp_pldm_len);
	zassert_equal(rc, 0);
	zassert_equal(resp_pldm_len, sizeof(struct pldm_msg_hdr) + 1U);

	assert_response_header(resp_msg, instance, unsupported_cmd);
	zassert_equal(resp_msg->payload[0], PLDM_ERROR_UNSUPPORTED_PLDM_CMD);
}

ZTEST_SUITE(pldm_platform, NULL, suite_setup, NULL, NULL, NULL);
