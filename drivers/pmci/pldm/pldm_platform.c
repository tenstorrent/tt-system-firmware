/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pldm_base.h"
#include "pldm_pdr.h"
#include "pldm_platform.h"

#include <zephyr/drivers/sensor.h>
#include <zephyr/rtio/rtio.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <libpldm/platform.h>

#include <limits.h>

#define PLDM_GET_PDR_MAX_DATA_LEN 48U
#define PLDM_SENSOR_READ_BUF_LEN  64U

static struct sensor_chan_spec pldm_sensor_channel[1];
static struct sensor_read_config pldm_sensor_read_cfg = {
	.sensor = NULL,
	.is_streaming = false,
	.channels = pldm_sensor_channel,
	.count = 1U,
	.max = 1U,
};
RTIO_IODEV_DEFINE(pldm_sensor_iodev, &__sensor_iodev_api, &pldm_sensor_read_cfg);
RTIO_DEFINE(pldm_sensor_rtio, 1, 1);
static uint8_t pldm_sensor_read_buf[PLDM_SENSOR_READ_BUF_LEN];

static const ver32_t pldm_platform_versions[] = {
	/* PLDM 1.1.0 followed by CRC32 over version entries. */
	{.alpha = 0x00, .update = 0xf0, .minor = 0xf1, .major = 0xf1},
	{.alpha = 0xba, .update = 0xbe, .minor = 0x9d, .major = 0x53},
};

static const bitfield8_t pldm_platform_commands[32] = {
	[PLDM_GET_SENSOR_READING / 8] = {.byte = BIT(PLDM_GET_SENSOR_READING % 8)},
	[PLDM_GET_PDR_REPOSITORY_INFO / 8] = {.byte = BIT(PLDM_GET_PDR_REPOSITORY_INFO % 8) |
						      BIT(PLDM_GET_PDR % 8)},
};

static const uint8_t pldm_zero_timestamp[PLDM_TIMESTAMP104_SIZE] = {0};

const ver32_t *pldm_platform_versions_get(size_t *versions_size)
{
	if (versions_size != NULL) {
		*versions_size = sizeof(pldm_platform_versions);
	}

	return pldm_platform_versions;
}

const bitfield8_t *pldm_platform_commands_get(void)
{
	return pldm_platform_commands;
}

static int pldm_handle_get_pdr_repository_info(const struct device *pdr_dev,
					       const struct pldm_header_info *req_hdr,
					       size_t req_payload_len, struct pldm_msg *resp_msg,
					       size_t *resp_pldm_len)
{
	struct pldm_pdr_repo_info repo_info;
	uint8_t cc;
	int rc;

	if (req_payload_len != 0U) {
		return pldm_cc_only_response(req_hdr, PLDM_ERROR_INVALID_LENGTH, resp_msg,
					     resp_pldm_len);
	}

	rc = pldm_pdr_get_repository_info(pdr_dev, &repo_info);
	if (rc != 0) {
		return pldm_cc_only_response(req_hdr, PLDM_ERROR, resp_msg, resp_pldm_len);
	}

	cc = encode_get_pdr_repository_info_resp(
		req_hdr->instance, PLDM_SUCCESS, PLDM_AVAILABLE, pldm_zero_timestamp,
		pldm_zero_timestamp, repo_info.record_count, (uint32_t)repo_info.repo_size,
		(uint32_t)repo_info.largest_record_size, PLDM_NO_TIMEOUT, resp_msg);
	if (cc != PLDM_SUCCESS) {
		return -EINVAL;
	}

	*resp_pldm_len = sizeof(struct pldm_msg_hdr) + PLDM_GET_PDR_REPOSITORY_INFO_RESP_BYTES;
	return 0;
}

static int pldm_handle_get_pdr(const struct device *pdr_dev, const struct pldm_header_info *req_hdr,
			       const struct pldm_msg *req_msg, size_t req_payload_len,
			       struct pldm_msg *resp_msg, size_t *resp_pldm_len)
{
	uint32_t record_handle;
	uint32_t data_transfer_handle;
	uint8_t transfer_op_flag;
	uint16_t request_count;
	uint16_t record_change_number;
	struct pldm_pdr_get_chunk_request chunk_req;
	struct pldm_pdr_get_chunk_response chunk_resp;
	uint8_t cc;
	int rc;

	cc = decode_get_pdr_req(req_msg, req_payload_len, &record_handle, &data_transfer_handle,
				&transfer_op_flag, &request_count, &record_change_number);
	if (cc != PLDM_SUCCESS) {
		return pldm_cc_only_response(req_hdr, cc, resp_msg, resp_pldm_len);
	}

	if (record_change_number != PLDM_PDR_RECORD_CHANGE_NUM) {
		return pldm_cc_only_response(req_hdr, PLDM_PLATFORM_INVALID_RECORD_CHANGE_NUMBER,
					     resp_msg, resp_pldm_len);
	}

	if (request_count == 0U) {
		return pldm_cc_only_response(req_hdr, PLDM_ERROR_INVALID_DATA, resp_msg,
					     resp_pldm_len);
	}

	if (transfer_op_flag != PLDM_GET_FIRSTPART && transfer_op_flag != PLDM_GET_NEXTPART) {
		return pldm_cc_only_response(req_hdr, PLDM_PLATFORM_INVALID_TRANSFER_OPERATION_FLAG,
					     resp_msg, resp_pldm_len);
	}

	chunk_req.record_handle = record_handle;
	chunk_req.data_transfer_handle = data_transfer_handle;
	chunk_req.transfer_op_flag = transfer_op_flag;
	chunk_req.request_count = request_count;
	chunk_req.max_response_count = PLDM_GET_PDR_MAX_DATA_LEN;

	rc = pldm_pdr_get_chunk(pdr_dev, &chunk_req, &chunk_resp);
	if (rc == -ENOENT) {
		return pldm_cc_only_response(req_hdr, PLDM_PLATFORM_INVALID_RECORD_HANDLE, resp_msg,
					     resp_pldm_len);
	}
	if (rc == -EBADMSG || rc == -ERANGE) {
		return pldm_cc_only_response(req_hdr, PLDM_PLATFORM_INVALID_DATA_TRANSFER_HANDLE,
					     resp_msg, resp_pldm_len);
	}
	if (rc == -EOPNOTSUPP) {
		return pldm_cc_only_response(req_hdr, PLDM_PLATFORM_INVALID_TRANSFER_OPERATION_FLAG,
					     resp_msg, resp_pldm_len);
	}
	if (rc != 0) {
		return pldm_cc_only_response(req_hdr, PLDM_ERROR, resp_msg, resp_pldm_len);
	}

	cc = encode_get_pdr_resp(req_hdr->instance, PLDM_SUCCESS, chunk_resp.next_record_handle,
				 chunk_resp.next_data_transfer_handle, chunk_resp.transfer_flag,
				 (uint16_t)chunk_resp.response_count, chunk_resp.record_data,
				 chunk_resp.transfer_crc, resp_msg);
	if (cc != PLDM_SUCCESS) {
		return -EINVAL;
	}

	const bool is_last = (chunk_resp.transfer_flag == PLDM_END) ||
			     (chunk_resp.transfer_flag == PLDM_START_AND_END);
	*resp_pldm_len = sizeof(struct pldm_msg_hdr) + PLDM_GET_PDR_MIN_RESP_BYTES +
			 chunk_resp.response_count + (is_last ? 1U : 0U);
	return 0;
}

static int pldm_sensor_value_to_s32(const struct sensor_value *value, int32_t *out,
				    int8_t unit_modifier)
{
	int64_t scaled_micro;
	int64_t rounded;

	if (value == NULL || out == NULL) {
		return -EINVAL;
	}

	/* Start with the value in micro-units (val1.val2 × 10⁶) */
	scaled_micro = ((int64_t)value->val1 * 1000000LL) + value->val2;

	/*
	 * Apply unit_modifier: the PLDM reading = physical_value × 10^(-unit_modifier).
	 * e.g. unit_modifier=-1 → reading = physical × 10, so multiply scaled_micro by 10
	 * before dividing back to whole counts.
	 */
	if (unit_modifier < 0) {
		for (int8_t i = 0; i > unit_modifier; i--) {
			scaled_micro *= 10LL;
		}
	} else if (unit_modifier > 0) {
		for (int8_t i = 0; i < unit_modifier; i++) {
			scaled_micro /= 10LL;
		}
	}

	rounded = (scaled_micro >= 0) ? ((scaled_micro + 500000LL) / 1000000LL)
				      : ((scaled_micro - 500000LL) / 1000000LL);

	if (rounded > INT32_MAX) {
		*out = INT32_MAX;
	} else if (rounded < INT32_MIN) {
		*out = INT32_MIN;
	} else {
		*out = (int32_t)rounded;
	}

	return 0;
}

static int pldm_sensor_read_decode(const struct pldm_pdr_numeric_sensor_desc *sensor_desc,
				   struct sensor_value *sensor_value)
{
	const struct sensor_decoder_api *decoder;
	struct sensor_q31_data q31_data;
	struct sensor_chan_spec channel = {
		.chan_type = sensor_desc->channel_type,
		.chan_idx = sensor_desc->channel_index,
	};
	size_t base_size;
	size_t frame_size;
	int rc;

	rc = sensor_get_decoder(sensor_desc->sensor, &decoder);
	if (rc != 0) {
		return rc;
	}

	rc = decoder->get_size_info(channel, &base_size, &frame_size);
	if (rc != 0) {
		return rc;
	}

	if (base_size > sizeof(pldm_sensor_read_buf)) {
		return -ENOMEM;
	}

	rc = sensor_reconfigure_read_iodev(&pldm_sensor_iodev, sensor_desc->sensor, &channel, 1U);
	if (rc != 0) {
		return rc;
	}

	rc = sensor_read(&pldm_sensor_iodev, &pldm_sensor_rtio, pldm_sensor_read_buf, base_size);
	if (rc != 0) {
		return rc;
	}

	struct sensor_decode_context decode_ctx = SENSOR_DECODE_CONTEXT_INIT(
		decoder, pldm_sensor_read_buf, channel.chan_type, channel.chan_idx);

	rc = sensor_decode(&decode_ctx, &q31_data, 1U);
	if (rc <= 0) {
		return (rc < 0) ? rc : -ENODATA;
	}

	if (q31_data.header.reading_count == 0U) {
		return -ENODATA;
	}

	if (q31_data.shift > 31) {
		return -ERANGE;
	}

	int64_t reading_micro =
		((int64_t)q31_data.readings[0].value * 1000000LL) >> (31 - q31_data.shift);

	rc = sensor_value_from_micro(sensor_value, reading_micro);
	if (rc != 0) {
		return rc;
	}

	return 0;
}

static int pldm_handle_get_sensor_reading(const struct device *pdr_dev,
					  const struct pldm_header_info *req_hdr,
					  const struct pldm_msg *req_msg, size_t req_payload_len,
					  struct pldm_msg *resp_msg, size_t *resp_pldm_len)
{
	uint16_t sensor_id;
	bool8_t rearm_event_state;
	const struct pldm_pdr_numeric_sensor_desc *sensor_desc;
	struct sensor_value sensor_value;
	int32_t reading;
	uint8_t present_reading[sizeof(int32_t)];
	size_t resp_payload_len;
	uint8_t cc;
	int rc;

	cc = decode_get_sensor_reading_req(req_msg, req_payload_len, &sensor_id,
					   &rearm_event_state);
	if (cc != PLDM_SUCCESS) {
		return pldm_cc_only_response(req_hdr, cc, resp_msg, resp_pldm_len);
	}

	if (rearm_event_state) {
		return pldm_cc_only_response(req_hdr,
					     PLDM_PLATFORM_REARM_UNAVAILABLE_IN_PRESENT_STATE,
					     resp_msg, resp_pldm_len);
	}

	sensor_desc = pldm_pdr_find_numeric_sensor_by_id(pdr_dev, sensor_id);
	if (sensor_desc == NULL) {
		return pldm_cc_only_response(req_hdr, PLDM_PLATFORM_INVALID_SENSOR_ID, resp_msg,
					     resp_pldm_len);
	}

	if (!device_is_ready(sensor_desc->sensor)) {
		return pldm_cc_only_response(req_hdr, PLDM_SENSOR_UNAVAILABLE, resp_msg,
					     resp_pldm_len);
	}

	rc = pldm_sensor_read_decode(sensor_desc, &sensor_value);
	if (rc != 0) {
		return pldm_cc_only_response(req_hdr, PLDM_ERROR, resp_msg, resp_pldm_len);
	}

	rc = pldm_sensor_value_to_s32(&sensor_value, &reading, sensor_desc->unit_modifier);
	if (rc != 0) {
		return pldm_cc_only_response(req_hdr, PLDM_ERROR, resp_msg, resp_pldm_len);
	}

	sys_put_le32((uint32_t)reading, present_reading);
	resp_payload_len = sizeof(struct pldm_get_sensor_reading_resp) - 1U + sizeof(int32_t);

	cc = encode_get_sensor_reading_resp(
		req_hdr->instance, PLDM_SUCCESS, PLDM_SENSOR_DATA_SIZE_SINT32, PLDM_SENSOR_ENABLED,
		PLDM_EVENTS_DISABLED, PLDM_SENSOR_NORMAL, PLDM_SENSOR_UNKNOWN, PLDM_SENSOR_UNKNOWN,
		present_reading, resp_msg, resp_payload_len);
	if (cc != PLDM_SUCCESS) {
		return -EINVAL;
	}

	*resp_pldm_len = sizeof(struct pldm_msg_hdr) + resp_payload_len;
	return 0;
}

int pldm_platform_build_response(const struct device *pdr_dev,
				 const struct pldm_header_info *req_hdr,
				 const struct pldm_msg *req_msg, size_t req_payload_len,
				 struct pldm_msg *resp_msg, size_t *resp_pldm_len)
{
	switch (req_hdr->command) {
	case PLDM_GET_PDR_REPOSITORY_INFO:
		return pldm_handle_get_pdr_repository_info(pdr_dev, req_hdr, req_payload_len,
							   resp_msg, resp_pldm_len);

	case PLDM_GET_PDR:
		return pldm_handle_get_pdr(pdr_dev, req_hdr, req_msg, req_payload_len, resp_msg,
					   resp_pldm_len);

	case PLDM_GET_SENSOR_READING:
		return pldm_handle_get_sensor_reading(pdr_dev, req_hdr, req_msg, req_payload_len,
						      resp_msg, resp_pldm_len);

	default:
		return pldm_cc_only_response(req_hdr, PLDM_ERROR_UNSUPPORTED_PLDM_CMD, resp_msg,
					     resp_pldm_len);
	}
}
