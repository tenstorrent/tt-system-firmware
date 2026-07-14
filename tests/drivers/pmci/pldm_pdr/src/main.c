/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <libpldm/edac.h>
#include <libpldm/platform.h>

#include <errno.h>
#include <string.h>

#include "pldm_pdr.h"

#define TEST_PDR_NODE DT_NODELABEL(pldm_pdr_test)

static const struct device *const pdr_dev = DEVICE_DT_GET(TEST_PDR_NODE);

static void *suite_setup(void)
{
	zassert_true(device_is_ready(pdr_dev));
	return NULL;
}

/* Reset any in-progress multi-chunk transfer before each test. */
static void before_each(void *f)
{
	struct pldm_pdr_get_chunk_request req = {
		.record_handle = 0U,
		.data_transfer_handle = 0U,
		.transfer_op_flag = PLDM_GET_FIRSTPART,
		.request_count = UINT16_MAX,
		.max_response_count = SIZE_MAX,
	};
	struct pldm_pdr_get_chunk_response resp;

	(void)pldm_pdr_get_chunk(pdr_dev, &req, &resp);
}

ZTEST(pldm_pdr_builder, test_repo_init_from_dts_provider)
{
	const uint16_t expected_numeric_sensor_record_len =
		PLDM_PDR_NUMERIC_SENSOR_PDR_FIXED_LENGTH + (3U * sizeof(uint32_t)) +
		(9U * sizeof(uint32_t));
	struct pldm_pdr_repo_info repo_info;
	struct pldm_pdr_record_metadata rec0;
	struct pldm_pdr_record_metadata rec1;
	struct pldm_pdr_record_metadata rec2;
	struct pldm_pdr_record_metadata rec3;
	struct pldm_pdr_record_metadata rec4;
	const struct pldm_terminus_locator_pdr *tl;
	const struct pldm_pdr_hdr *hdr1;
	const struct pldm_pdr_hdr *hdr2;
	const struct pldm_pdr_hdr *hdr3;
	const struct pldm_pdr_hdr *hdr4;
	const struct pldm_sensor_auxiliary_names_pdr *aux_pdr;
	const uint8_t *names;
	struct pldm_numeric_sensor_value_pdr sensor_pdr;
	int rc;

	rc = pldm_pdr_get_repository_info(pdr_dev, &repo_info);
	zassert_equal(rc, 0);

	/* 1 terminus locator + 3 numeric sensor records + 1 aux-name record. */
	zassert_equal(repo_info.record_count, 5U);
	zassert_true(repo_info.repo_size >= sizeof(struct pldm_terminus_locator_pdr));
	zassert_true(repo_info.largest_record_size > 0U);

	rc = pldm_pdr_get_record_at_index(pdr_dev, 0U, &rec0);
	zassert_equal(rc, 0);
	zassert_equal(rec0.record_handle, 1U);
	zassert_equal(rec0.next_record_handle, 2U);

	rc = pldm_pdr_get_record_at_index(pdr_dev, 1U, &rec1);
	zassert_equal(rc, 0);
	zassert_equal(rec1.record_handle, 2U);
	zassert_equal(rec1.next_record_handle, 3U);

	rc = pldm_pdr_get_record_at_index(pdr_dev, 2U, &rec2);
	zassert_equal(rc, 0);
	zassert_equal(rec2.record_handle, 3U);
	zassert_equal(rec2.next_record_handle, 4U);

	rc = pldm_pdr_get_record_at_index(pdr_dev, 3U, &rec3);
	zassert_equal(rc, 0);
	zassert_equal(rec3.record_handle, 4U);
	zassert_equal(rec3.next_record_handle, 5U);

	rc = pldm_pdr_get_record_at_index(pdr_dev, 4U, &rec4);
	zassert_equal(rc, 0);
	zassert_equal(rec4.record_handle, 5U);
	zassert_equal(rec4.next_record_handle, 0U);

	tl = (const struct pldm_terminus_locator_pdr *)rec0.record_data;
	zassert_equal(tl->hdr.type, PLDM_TERMINUS_LOCATOR_PDR);
	zassert_equal(tl->tid, 0U);
	zassert_equal(tl->terminus_locator_value[0], 0U);

	hdr1 = (const struct pldm_pdr_hdr *)rec1.record_data;
	hdr2 = (const struct pldm_pdr_hdr *)rec2.record_data;
	hdr3 = (const struct pldm_pdr_hdr *)rec3.record_data;
	hdr4 = (const struct pldm_pdr_hdr *)rec4.record_data;

	zassert_equal(sys_le32_to_cpu(tl->hdr.record_handle), rec0.record_handle);
	zassert_equal(tl->hdr.version, 1U);
	zassert_equal(sys_le16_to_cpu(tl->hdr.record_change_num), PLDM_PDR_RECORD_CHANGE_NUM);

	zassert_equal(sys_le32_to_cpu(hdr1->record_handle), rec1.record_handle);
	zassert_equal(hdr1->version, 1U);
	zassert_equal(sys_le16_to_cpu(hdr1->record_change_num), PLDM_PDR_RECORD_CHANGE_NUM);

	zassert_equal(sys_le32_to_cpu(hdr2->record_handle), rec2.record_handle);
	zassert_equal(hdr2->version, 1U);
	zassert_equal(sys_le16_to_cpu(hdr2->record_change_num), PLDM_PDR_RECORD_CHANGE_NUM);

	zassert_equal(sys_le32_to_cpu(hdr3->record_handle), rec3.record_handle);
	zassert_equal(hdr3->version, 1U);
	zassert_equal(sys_le16_to_cpu(hdr3->record_change_num), PLDM_PDR_RECORD_CHANGE_NUM);

	zassert_equal(sys_le32_to_cpu(hdr4->record_handle), rec4.record_handle);
	zassert_equal(hdr4->version, 1U);
	zassert_equal(sys_le16_to_cpu(hdr4->record_change_num), PLDM_PDR_RECORD_CHANGE_NUM);

	zassert_equal(hdr1->type, PLDM_NUMERIC_SENSOR_PDR);
	zassert_equal(hdr2->type, PLDM_SENSOR_AUXILIARY_NAMES_PDR);
	zassert_equal(hdr3->type, PLDM_NUMERIC_SENSOR_PDR);
	zassert_equal(hdr4->type, PLDM_NUMERIC_SENSOR_PDR);

	aux_pdr = (const struct pldm_sensor_auxiliary_names_pdr *)hdr2;
	zassert_equal(sys_le16_to_cpu(aux_pdr->sensor_id), 101U);
	zassert_equal(aux_pdr->sensor_count, 1U);
	names = aux_pdr->names;
	zassert_equal(names[0], 1U);
	zassert_equal(names[1], 'e');
	zassert_equal(names[2], 'n');
	zassert_equal(names[3], '\0');
	/* UTF-16BE "ASIC_TEMP" + null terminator. */
	zassert_equal(names[4], 0U);
	zassert_equal(names[5], 'A');
	zassert_equal(names[6], 0U);
	zassert_equal(names[7], 'S');
	zassert_equal(names[8], 0U);
	zassert_equal(names[9], 'I');
	zassert_equal(names[10], 0U);
	zassert_equal(names[11], 'C');
	zassert_equal(names[12], 0U);
	zassert_equal(names[13], '_');
	zassert_equal(names[14], 0U);
	zassert_equal(names[15], 'T');
	zassert_equal(names[16], 0U);
	zassert_equal(names[17], 'E');
	zassert_equal(names[18], 0U);
	zassert_equal(names[19], 'M');
	zassert_equal(names[20], 0U);
	zassert_equal(names[21], 'P');
	zassert_equal(names[22], 0U);
	zassert_equal(names[23], 0U);

	zassert_equal(rec1.record_len, expected_numeric_sensor_record_len,
		      "unexpected numeric record length: %u", (uint32_t)rec1.record_len);
	rc = decode_numeric_sensor_pdr_data(rec1.record_data, rec1.record_len, &sensor_pdr);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(sensor_pdr.sensor_id, 101U);
	zassert_equal(sensor_pdr.base_unit, PLDM_SENSOR_UNIT_DEGRESS_C);
	zassert_equal(sensor_pdr.unit_modifier, -1);
	zassert_equal(sensor_pdr.sensor_data_size, PLDM_SENSOR_DATA_SIZE_SINT32);
	zassert_equal(sensor_pdr.range_field_format, PLDM_RANGE_FIELD_FORMAT_SINT32);

	zassert_equal(rec3.record_len, expected_numeric_sensor_record_len,
		      "unexpected numeric record length: %u", (uint32_t)rec3.record_len);
	rc = decode_numeric_sensor_pdr_data(rec3.record_data, rec3.record_len, &sensor_pdr);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(sensor_pdr.sensor_id, 202U);
	zassert_equal(sensor_pdr.base_unit, PLDM_SENSOR_UNIT_WATTS);
	zassert_equal(sensor_pdr.unit_modifier, 0);
	zassert_equal(sensor_pdr.sensor_data_size, PLDM_SENSOR_DATA_SIZE_SINT32);
	zassert_equal(sensor_pdr.range_field_format, PLDM_RANGE_FIELD_FORMAT_SINT32);

	zassert_equal(rec4.record_len, expected_numeric_sensor_record_len,
		      "unexpected numeric record length: %u", (uint32_t)rec4.record_len);
	rc = decode_numeric_sensor_pdr_data(rec4.record_data, rec4.record_len, &sensor_pdr);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(sensor_pdr.sensor_id, 303U);
	zassert_equal(sensor_pdr.entity_type, 20U);
	zassert_equal(sensor_pdr.entity_instance, 3U);
	zassert_equal(sensor_pdr.container_id, 7U);
	/* Explicit DT base-unit should override channel-type inferred unit. */
	zassert_equal(sensor_pdr.base_unit, 77U);
	zassert_equal(sensor_pdr.sensor_data_size, PLDM_SENSOR_DATA_SIZE_SINT32);
	zassert_equal(sensor_pdr.range_field_format, PLDM_RANGE_FIELD_FORMAT_SINT32);
}

ZTEST(pldm_pdr_builder, test_update_tid_eid_updates_terminus_locator_record)
{
	struct pldm_pdr_record_metadata record_meta;
	const struct pldm_terminus_locator_pdr *tl;
	int rc;

	rc = pldm_pdr_update_tid_eid(pdr_dev, 0x33U, 0x44U);
	zassert_equal(rc, 0);

	rc = pldm_pdr_get_record_at_index(pdr_dev, 0U, &record_meta);
	zassert_equal(rc, 0);
	zassert_equal(record_meta.record_handle, 1U);
	zassert_equal(record_meta.next_record_handle, 2U);

	tl = (const struct pldm_terminus_locator_pdr *)record_meta.record_data;
	zassert_equal(tl->hdr.type, PLDM_TERMINUS_LOCATOR_PDR);
	zassert_equal(tl->tid, 0x33U);
	zassert_equal(tl->terminus_locator_value[0], 0x44U);
}

ZTEST(pldm_pdr_builder, test_update_tid_eid_rejects_null_device)
{
	int rc;

	rc = pldm_pdr_update_tid_eid(NULL, 0x33U, 0x44U);
	zassert_equal(rc, -EINVAL);
}

ZTEST(pldm_pdr_builder, test_get_chunk_first_and_next)
{
	struct pldm_pdr_record_metadata first_record;
	struct pldm_pdr_get_chunk_request req;
	struct pldm_pdr_get_chunk_response resp;
	int rc;

	rc = pldm_pdr_get_record_at_index(pdr_dev, 0U, &first_record);
	zassert_equal(rc, 0);
	zassert_equal(first_record.record_handle, 1U);

	req.record_handle = 0U;
	req.data_transfer_handle = 0U;
	req.transfer_op_flag = PLDM_GET_FIRSTPART;
	req.request_count = 1U;
	req.max_response_count = 1U;

	rc = pldm_pdr_get_chunk(pdr_dev, &req, &resp);
	zassert_equal(rc, 0);
	zassert_equal(resp.transfer_flag, PLDM_START);
	zassert_equal(resp.next_record_handle, first_record.next_record_handle);
	zassert_equal(resp.next_data_transfer_handle, 1U);
	zassert_equal(resp.response_count, 1U);
	zassert_equal(resp.transfer_crc, 0U);
	zassert_equal(resp.record_data[0], first_record.record_data[0]);

	req.record_handle = first_record.record_handle;
	req.data_transfer_handle = resp.next_data_transfer_handle;
	req.transfer_op_flag = PLDM_GET_NEXTPART;
	req.request_count = 1U;
	req.max_response_count = 1U;

	rc = pldm_pdr_get_chunk(pdr_dev, &req, &resp);
	zassert_equal(rc, 0);
}

ZTEST(pldm_pdr_builder, test_get_chunk_invalid_next_part_state)
{
	struct pldm_pdr_get_chunk_request req = {
		.record_handle = 1U,
		.data_transfer_handle = 0U,
		.transfer_op_flag = PLDM_GET_NEXTPART,
		.request_count = 1U,
		.max_response_count = 1U,
	};
	struct pldm_pdr_get_chunk_response resp;
	int rc;

	rc = pldm_pdr_get_chunk(pdr_dev, &req, &resp);
	zassert_equal(rc, -EBADMSG);
}

ZTEST(pldm_pdr_builder, test_get_repository_info_rejects_null_args)
{
	struct pldm_pdr_repo_info repo_info;
	int rc;

	rc = pldm_pdr_get_repository_info(NULL, &repo_info);
	zassert_equal(rc, -EINVAL);

	rc = pldm_pdr_get_repository_info(pdr_dev, NULL);
	zassert_equal(rc, -EINVAL);
}

ZTEST(pldm_pdr_builder, test_get_record_at_index_rejects_null_args)
{
	struct pldm_pdr_record_metadata meta;
	int rc;

	rc = pldm_pdr_get_record_at_index(NULL, 0U, &meta);
	zassert_equal(rc, -EINVAL);

	rc = pldm_pdr_get_record_at_index(pdr_dev, 0U, NULL);
	zassert_equal(rc, -EINVAL);
}

ZTEST(pldm_pdr_builder, test_get_record_at_index_out_of_range)
{
	struct pldm_pdr_repo_info repo_info;
	struct pldm_pdr_record_metadata meta;
	int rc;

	rc = pldm_pdr_get_repository_info(pdr_dev, &repo_info);
	zassert_equal(rc, 0);

	rc = pldm_pdr_get_record_at_index(pdr_dev, repo_info.record_count, &meta);
	zassert_equal(rc, -ERANGE);
}

ZTEST(pldm_pdr_builder, test_find_record_index_found)
{
	uint32_t idx;
	int rc;

	/* Handles are 1-based and sequential; indices are 0-based. */
	for (uint32_t handle = 1U; handle <= 5U; handle++) {
		rc = pldm_pdr_find_record_index(pdr_dev, handle, &idx);
		zassert_equal(rc, 0);
		zassert_equal(idx, handle - 1U);
	}
}

ZTEST(pldm_pdr_builder, test_find_record_index_not_found)
{
	uint32_t idx;
	int rc;

	rc = pldm_pdr_find_record_index(pdr_dev, 999U, &idx);
	zassert_equal(rc, -ENOENT);
}

ZTEST(pldm_pdr_builder, test_find_numeric_sensor_by_id)
{
	const struct pldm_pdr_numeric_sensor_desc *desc;

	desc = pldm_pdr_find_numeric_sensor_by_id(pdr_dev, 101U);
	zassert_not_null(desc);
	zassert_equal(desc->sensor_id, 101U);

	desc = pldm_pdr_find_numeric_sensor_by_id(pdr_dev, 202U);
	zassert_not_null(desc);
	zassert_equal(desc->sensor_id, 202U);

	desc = pldm_pdr_find_numeric_sensor_by_id(pdr_dev, 303U);
	zassert_not_null(desc);
	zassert_equal(desc->sensor_id, 303U);

	desc = pldm_pdr_find_numeric_sensor_by_id(pdr_dev, 999U);
	zassert_is_null(desc);

	desc = pldm_pdr_find_numeric_sensor_by_id(NULL, 101U);
	zassert_is_null(desc);
}

ZTEST(pldm_pdr_builder, test_get_chunk_complete_single_transfer)
{
	struct pldm_pdr_record_metadata rec;
	struct pldm_pdr_get_chunk_request req;
	struct pldm_pdr_get_chunk_response resp;
	int rc;

	rc = pldm_pdr_get_record_at_index(pdr_dev, 0U, &rec);
	zassert_equal(rc, 0);

	req.record_handle = rec.record_handle;
	req.data_transfer_handle = 0U;
	req.transfer_op_flag = PLDM_GET_FIRSTPART;
	req.request_count = UINT16_MAX;
	req.max_response_count = SIZE_MAX;

	rc = pldm_pdr_get_chunk(pdr_dev, &req, &resp);
	zassert_equal(rc, 0);
	zassert_equal(resp.transfer_flag, PLDM_START_AND_END);
	zassert_equal(resp.response_count, rec.record_len);
	zassert_equal(resp.next_data_transfer_handle, 0U);
	zassert_equal(resp.record_data, rec.record_data);
	zassert_equal(resp.transfer_crc, pldm_edac_crc8(rec.record_data, rec.record_len));
}

ZTEST(pldm_pdr_builder, test_get_chunk_by_record_handle)
{
	struct pldm_pdr_record_metadata rec;
	struct pldm_pdr_get_chunk_request req;
	struct pldm_pdr_get_chunk_response resp;
	int rc;

	rc = pldm_pdr_get_record_at_index(pdr_dev, 2U, &rec);
	zassert_equal(rc, 0);
	zassert_equal(rec.record_handle, 3U);

	req.record_handle = 3U;
	req.data_transfer_handle = 0U;
	req.transfer_op_flag = PLDM_GET_FIRSTPART;
	req.request_count = UINT16_MAX;
	req.max_response_count = SIZE_MAX;

	rc = pldm_pdr_get_chunk(pdr_dev, &req, &resp);
	zassert_equal(rc, 0);
	zassert_equal(resp.transfer_flag, PLDM_START_AND_END);
	zassert_equal(resp.next_record_handle, rec.next_record_handle);
	zassert_equal(resp.record_data, rec.record_data);
}

ZTEST(pldm_pdr_builder, test_get_chunk_unknown_record_handle)
{
	struct pldm_pdr_get_chunk_request req = {
		.record_handle = 999U,
		.data_transfer_handle = 0U,
		.transfer_op_flag = PLDM_GET_FIRSTPART,
		.request_count = 1U,
		.max_response_count = 1U,
	};
	struct pldm_pdr_get_chunk_response resp;
	int rc;

	rc = pldm_pdr_get_chunk(pdr_dev, &req, &resp);
	zassert_equal(rc, -ENOENT);
}

ZTEST(pldm_pdr_builder, test_get_chunk_unsupported_op_flag)
{
	struct pldm_pdr_get_chunk_request req = {
		.record_handle = 0U,
		.data_transfer_handle = 0U,
		.transfer_op_flag = 0xFFU,
		.request_count = 1U,
		.max_response_count = 1U,
	};
	struct pldm_pdr_get_chunk_response resp;
	int rc;

	rc = pldm_pdr_get_chunk(pdr_dev, &req, &resp);
	zassert_equal(rc, -EOPNOTSUPP);
}

ZTEST(pldm_pdr_builder, test_get_chunk_invalid_args)
{
	struct pldm_pdr_get_chunk_request req = {
		.record_handle = 0U,
		.data_transfer_handle = 0U,
		.transfer_op_flag = PLDM_GET_FIRSTPART,
		.request_count = 1U,
		.max_response_count = 1U,
	};
	struct pldm_pdr_get_chunk_response resp;
	int rc;

	rc = pldm_pdr_get_chunk(NULL, &req, &resp);
	zassert_equal(rc, -EINVAL);

	rc = pldm_pdr_get_chunk(pdr_dev, NULL, &resp);
	zassert_equal(rc, -EINVAL);

	rc = pldm_pdr_get_chunk(pdr_dev, &req, NULL);
	zassert_equal(rc, -EINVAL);

	req.request_count = 0U;
	rc = pldm_pdr_get_chunk(pdr_dev, &req, &resp);
	zassert_equal(rc, -EINVAL);
}

ZTEST(pldm_pdr_builder, test_get_chunk_nextpart_stale_handles)
{
	struct pldm_pdr_get_chunk_request req;
	struct pldm_pdr_get_chunk_response resp;
	int rc;

	/* Establish an active multi-chunk transfer (1 byte at a time). */
	req.record_handle = 0U;
	req.data_transfer_handle = 0U;
	req.transfer_op_flag = PLDM_GET_FIRSTPART;
	req.request_count = 1U;
	req.max_response_count = 1U;

	rc = pldm_pdr_get_chunk(pdr_dev, &req, &resp);
	zassert_equal(rc, 0);
	zassert_equal(resp.transfer_flag, PLDM_START);

	/* Wrong record handle. */
	req.record_handle = 99U;
	req.data_transfer_handle = resp.next_data_transfer_handle;
	req.transfer_op_flag = PLDM_GET_NEXTPART;
	rc = pldm_pdr_get_chunk(pdr_dev, &req, &resp);
	zassert_equal(rc, -EBADMSG);

	/* Correct record handle but wrong data transfer handle. */
	req.record_handle = 1U;
	req.data_transfer_handle = 99U;
	rc = pldm_pdr_get_chunk(pdr_dev, &req, &resp);
	zassert_equal(rc, -EBADMSG);
}

ZTEST_SUITE(pldm_pdr_builder, NULL, suite_setup, before_each, NULL, NULL);
