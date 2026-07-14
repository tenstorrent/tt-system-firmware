/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_pldm_pdr

#include "pldm_pdr.h"

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <libpldm/edac.h>
#include <libpldm/platform.h>

#include <errno.h>
#include <string.h>

#define PLDM_PDR_TERMINUS_HANDLE 0x0001U
#define PLDM_PDR_RECORD_HANDLE   0x00000001U
#define PLDM_PDR_SENSOR_AUX_LANG "en"
#define PLDM_PDR_NUMERIC_SENSOR_RECORD_WIRE_SIZE                                                   \
	(PLDM_PDR_NUMERIC_SENSOR_PDR_FIXED_LENGTH + (3U * sizeof(uint32_t)) +                      \
	 (9U * sizeof(uint32_t)))
#define APPEND(_expr)                                                                              \
	do {                                                                                       \
		rc = (_expr);                                                                      \
		if (rc != 0) {                                                                     \
			return rc;                                                                 \
		}                                                                                  \
	} while (false)
/* clang-format off */
#define PLDM_PDR_BASE_UNIT_FROM_CHANNEL_TYPE(channel_type)                                      \
	(((channel_type) == SENSOR_CHAN_DIE_TEMP ||                                             \
	  (channel_type) == SENSOR_CHAN_AMBIENT_TEMP ||                                         \
	  (channel_type) == SENSOR_CHAN_GAUGE_TEMP) ? PLDM_SENSOR_UNIT_DEGRESS_C :              \
	 ((channel_type) == SENSOR_CHAN_VOLTAGE ||                                              \
	  (channel_type) == SENSOR_CHAN_VSHUNT ||                                               \
	  (channel_type) == SENSOR_CHAN_GAUGE_VOLTAGE) ? PLDM_SENSOR_UNIT_VOLTS :               \
	 ((channel_type) == SENSOR_CHAN_CURRENT ||                                              \
	  (channel_type) == SENSOR_CHAN_GAUGE_AVG_CURRENT ||                                    \
	  (channel_type) == SENSOR_CHAN_GAUGE_STDBY_CURRENT ||                                  \
	  (channel_type) == SENSOR_CHAN_GAUGE_MAX_LOAD_CURRENT) ? PLDM_SENSOR_UNIT_AMPS :       \
	 ((channel_type) == SENSOR_CHAN_POWER) ? PLDM_SENSOR_UNIT_WATTS :                       \
	 ((channel_type) == SENSOR_CHAN_PRESS) ? PLDM_SENSOR_UNIT_KPA :                         \
	 ((channel_type) == SENSOR_CHAN_HUMIDITY ||                                             \
	  (channel_type) == SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) ? PLDM_SENSOR_UNIT_PERCENTAGE :  \
	 ((channel_type) == SENSOR_CHAN_AMBIENT_LIGHT ||                                        \
	  (channel_type) == SENSOR_CHAN_LIGHT ||                                                \
	  (channel_type) == SENSOR_CHAN_IR ||                                                   \
	  (channel_type) == SENSOR_CHAN_RED ||                                                  \
	  (channel_type) == SENSOR_CHAN_GREEN ||                                                \
	  (channel_type) == SENSOR_CHAN_BLUE) ? PLDM_SENSOR_UNIT_LUX :                          \
	 ((channel_type) == SENSOR_CHAN_FREQUENCY) ? PLDM_SENSOR_UNIT_HERTZ :                   \
	 ((channel_type) == SENSOR_CHAN_RPM) ? PLDM_SENSOR_UNIT_RPM :                           \
	 ((channel_type) == SENSOR_CHAN_RESISTANCE ||                                           \
	  (channel_type) == SENSOR_CHAN_GAS_RES) ? PLDM_SENSOR_UNIT_OHMS :                      \
	 ((channel_type) == SENSOR_CHAN_ALTITUDE ||                                             \
	  (channel_type) == SENSOR_CHAN_DISTANCE) ? PLDM_SENSOR_UNIT_METERS :                   \
	 ((channel_type) == SENSOR_CHAN_CO2 ||                                                  \
	  (channel_type) == SENSOR_CHAN_O2) ? PLDM_SENSOR_UNIT_PPM :                            \
	 PLDM_SENSOR_UNIT_NONE)
/* clang-format on */

struct pldm_pdr_record_info {
	uint32_t handle;
	size_t offset;
	size_t length;
};

struct pldm_pdr_provider_config {
	const struct pldm_pdr_numeric_sensor_desc *sensors;
	size_t sensor_count;
	struct pldm_pdr_record_info *pdr_records;
	size_t pdr_record_capacity;
	uint8_t *pdr_repo;
	size_t pdr_repo_capacity;
};

struct pldm_pdr_provider_data {
	uint32_t pdr_record_count;
	size_t pdr_repo_size;
	size_t pdr_largest_record_size;
	bool transfer_active;
	uint32_t transfer_record_handle;
	uint32_t transfer_record_index;
	uint32_t transfer_next_data_handle;
};

static int pldm_pdr_append_record(const struct pldm_pdr_provider_config *cfg,
				  struct pldm_pdr_provider_data *data, uint32_t record_handle,
				  const void *record_data, size_t record_len);

int pldm_pdr_update_tid_eid(const struct device *pdr_dev, uint8_t tid, uint8_t endpoint_id)
{
	const struct pldm_pdr_provider_config *cfg;
	struct pldm_terminus_locator_pdr *tl;
	const struct pldm_pdr_record_info *tl_record;

	if (pdr_dev == NULL) {
		return -EINVAL;
	}

	if (!device_is_ready(pdr_dev)) {
		return -ENODEV;
	}

	cfg = pdr_dev->config;
	tl_record = &cfg->pdr_records[0];
	if (tl_record->length < sizeof(struct pldm_terminus_locator_pdr) ||
	    cfg->pdr_repo_capacity < sizeof(struct pldm_terminus_locator_pdr) ||
	    tl_record->offset >
		    (cfg->pdr_repo_capacity - sizeof(struct pldm_terminus_locator_pdr))) {
		return -EINVAL;
	}

	tl = (struct pldm_terminus_locator_pdr *)&cfg->pdr_repo[tl_record->offset];
	if (tl->hdr.type != PLDM_TERMINUS_LOCATOR_PDR) {
		return -EINVAL;
	}

	tl->tid = tid;
	tl->terminus_locator_value[0] = endpoint_id;

	return 0;
}

static size_t pldm_pdr_encode_utf16be_terminated(const char *src, uint8_t *dst, size_t dst_len)
{
	size_t out = 0U;

	for (size_t i = 0U; src[i] != '\0'; i++) {
		if ((out + 2U) > dst_len) {
			return 0U;
		}

		/* PLDM auxiliary names use UTF-16BE; current DT names are ASCII. */
		dst[out++] = 0U;
		dst[out++] = (uint8_t)src[i];
	}

	if ((out + 2U) > dst_len) {
		return 0U;
	}

	dst[out++] = 0U;
	dst[out++] = 0U;

	return out;
}

static int pldm_pdr_buf_append_u8(uint8_t **cursor, size_t *remaining, uint8_t value)
{
	if (*remaining < sizeof(value)) {
		return -EOVERFLOW;
	}

	*(*cursor)++ = value;
	*remaining -= sizeof(value);

	return 0;
}

static int pldm_pdr_buf_append_u16(uint8_t **cursor, size_t *remaining, uint16_t value)
{
	if (*remaining < sizeof(value)) {
		return -EOVERFLOW;
	}

	sys_put_le16(value, *cursor);
	*cursor += sizeof(value);
	*remaining -= sizeof(value);

	return 0;
}

static int pldm_pdr_buf_append_u32(uint8_t **cursor, size_t *remaining, uint32_t value)
{
	if (*remaining < sizeof(value)) {
		return -EOVERFLOW;
	}

	sys_put_le32(value, *cursor);
	*cursor += sizeof(value);
	*remaining -= sizeof(value);

	return 0;
}

static int pldm_pdr_buf_append_s32(uint8_t **cursor, size_t *remaining, int32_t value)
{
	return pldm_pdr_buf_append_u32(cursor, remaining, (uint32_t)value);
}

static int pldm_pdr_buf_append_f32(uint8_t **cursor, size_t *remaining, real32_t value)
{
	uint32_t raw = 0U;

	BUILD_ASSERT(sizeof(raw) == sizeof(value), "unexpected float size");
	memcpy(&raw, &value, sizeof(raw));

	return pldm_pdr_buf_append_u32(cursor, remaining, raw);
}

static int pldm_pdr_encode_numeric_sensor_record(const struct pldm_numeric_sensor_value_pdr *pdr,
						 uint8_t *dst, size_t dst_len, size_t *record_len)
{
	uint8_t *cursor;
	size_t remaining;
	int rc;

	if (pdr == NULL || dst == NULL || record_len == NULL) {
		return -EINVAL;
	}

	cursor = dst;
	remaining = dst_len;

	APPEND(pldm_pdr_buf_append_u32(&cursor, &remaining, pdr->hdr.record_handle));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->hdr.version));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->hdr.type));
	APPEND(pldm_pdr_buf_append_u16(&cursor, &remaining, pdr->hdr.record_change_num));
	APPEND(pldm_pdr_buf_append_u16(&cursor, &remaining, pdr->hdr.length));

	APPEND(pldm_pdr_buf_append_u16(&cursor, &remaining, pdr->terminus_handle));
	APPEND(pldm_pdr_buf_append_u16(&cursor, &remaining, pdr->sensor_id));
	APPEND(pldm_pdr_buf_append_u16(&cursor, &remaining, pdr->entity_type));
	APPEND(pldm_pdr_buf_append_u16(&cursor, &remaining, pdr->entity_instance));
	APPEND(pldm_pdr_buf_append_u16(&cursor, &remaining, pdr->container_id));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->sensor_init));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->sensor_auxiliary_names_pdr));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->base_unit));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, (uint8_t)pdr->unit_modifier));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->rate_unit));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->base_oem_unit_handle));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->aux_unit));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, (uint8_t)pdr->aux_unit_modifier));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->aux_rate_unit));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->rel));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->aux_oem_unit_handle));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->is_linear));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->sensor_data_size));
	APPEND(pldm_pdr_buf_append_f32(&cursor, &remaining, pdr->resolution));
	APPEND(pldm_pdr_buf_append_f32(&cursor, &remaining, pdr->offset));
	APPEND(pldm_pdr_buf_append_u16(&cursor, &remaining, pdr->accuracy));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->plus_tolerance));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->minus_tolerance));
	APPEND(pldm_pdr_buf_append_s32(&cursor, &remaining, pdr->hysteresis.value_s32));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->supported_thresholds.byte));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining,
				      pdr->threshold_and_hysteresis_volatility.byte));
	APPEND(pldm_pdr_buf_append_f32(&cursor, &remaining, pdr->state_transition_interval));
	APPEND(pldm_pdr_buf_append_f32(&cursor, &remaining, pdr->update_interval));
	APPEND(pldm_pdr_buf_append_s32(&cursor, &remaining, pdr->max_readable.value_s32));
	APPEND(pldm_pdr_buf_append_s32(&cursor, &remaining, pdr->min_readable.value_s32));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->range_field_format));
	APPEND(pldm_pdr_buf_append_u8(&cursor, &remaining, pdr->range_field_support.byte));
	APPEND(pldm_pdr_buf_append_s32(&cursor, &remaining, pdr->nominal_value.value_s32));
	APPEND(pldm_pdr_buf_append_s32(&cursor, &remaining, pdr->normal_max.value_s32));
	APPEND(pldm_pdr_buf_append_s32(&cursor, &remaining, pdr->normal_min.value_s32));
	APPEND(pldm_pdr_buf_append_s32(&cursor, &remaining, pdr->warning_high.value_s32));
	APPEND(pldm_pdr_buf_append_s32(&cursor, &remaining, pdr->warning_low.value_s32));
	APPEND(pldm_pdr_buf_append_s32(&cursor, &remaining, pdr->critical_high.value_s32));
	APPEND(pldm_pdr_buf_append_s32(&cursor, &remaining, pdr->critical_low.value_s32));
	APPEND(pldm_pdr_buf_append_s32(&cursor, &remaining, pdr->fatal_high.value_s32));
	APPEND(pldm_pdr_buf_append_s32(&cursor, &remaining, pdr->fatal_low.value_s32));

	*record_len = (size_t)(cursor - dst);

	return 0;
}

static int pldm_pdr_append_sensor_aux_name_record(const struct pldm_pdr_provider_config *cfg,
						  struct pldm_pdr_provider_data *data,
						  uint32_t record_handle, uint16_t sensor_id,
						  const char *sensor_name)
{
	struct pldm_sensor_auxiliary_names_pdr *pdr;
	uint8_t *names;
	const size_t lang_len = strlen(PLDM_PDR_SENSOR_AUX_LANG);
	const size_t names_prefix_len = 1U + lang_len + 1U;
	size_t remaining_repo_len;
	size_t max_names_len;
	size_t utf16_len;
	size_t names_len;
	size_t record_len;
	size_t pdr_payload_len;

	if (data->pdr_record_count >= cfg->pdr_record_capacity) {
		return -ENOMEM;
	}

	if (data->pdr_repo_size > cfg->pdr_repo_capacity) {
		return -ENOMEM;
	}

	remaining_repo_len = cfg->pdr_repo_capacity - data->pdr_repo_size;
	if (remaining_repo_len < sizeof(*pdr)) {
		return -ENOMEM;
	}

	max_names_len = remaining_repo_len - (sizeof(*pdr) - 1U);
	if (max_names_len < names_prefix_len) {
		return -ENOMEM;
	}

	pdr = (struct pldm_sensor_auxiliary_names_pdr *)&cfg->pdr_repo[data->pdr_repo_size];
	names = pdr->names;

	/* One sensor name list with one locale entry. */
	names[0] = 1U;
	memcpy(&names[1], PLDM_PDR_SENSOR_AUX_LANG, lang_len);
	names[1U + lang_len] = '\0';
	utf16_len = pldm_pdr_encode_utf16be_terminated(sensor_name, &names[1U + lang_len + 1U],
						       max_names_len - names_prefix_len);
	if (utf16_len == 0U) {
		return -ENOMEM;
	}

	names_len = names_prefix_len + utf16_len;
	record_len = sizeof(*pdr) - 1U + names_len;
	pdr_payload_len = sizeof(*pdr) - sizeof(struct pldm_pdr_hdr) - 1U + names_len;
	if (pdr_payload_len > UINT16_MAX) {
		return -EOVERFLOW;
	}

	pdr->hdr.record_handle = sys_cpu_to_le32(record_handle);
	pdr->hdr.version = 1U;
	pdr->hdr.type = PLDM_SENSOR_AUXILIARY_NAMES_PDR;
	pdr->hdr.record_change_num = sys_cpu_to_le16(PLDM_PDR_RECORD_CHANGE_NUM);
	pdr->hdr.length = sys_cpu_to_le16((uint16_t)pdr_payload_len);
	pdr->terminus_handle = sys_cpu_to_le16(PLDM_PDR_TERMINUS_HANDLE);
	pdr->sensor_id = sys_cpu_to_le16(sensor_id);
	pdr->sensor_count = 1U;
	cfg->pdr_records[data->pdr_record_count].handle = record_handle;
	cfg->pdr_records[data->pdr_record_count].offset = data->pdr_repo_size;
	cfg->pdr_records[data->pdr_record_count].length = record_len;
	data->pdr_record_count++;
	data->pdr_repo_size += record_len;
	data->pdr_largest_record_size = MAX(data->pdr_largest_record_size, record_len);

	return 0;
}

static int pldm_pdr_append_record(const struct pldm_pdr_provider_config *cfg,
				  struct pldm_pdr_provider_data *data, uint32_t record_handle,
				  const void *record_data, size_t record_len)
{
	if (data->pdr_record_count >= cfg->pdr_record_capacity) {
		return -ENOMEM;
	}

	if ((data->pdr_repo_size + record_len) > cfg->pdr_repo_capacity) {
		return -ENOMEM;
	}

	memcpy(&cfg->pdr_repo[data->pdr_repo_size], record_data, record_len);
	cfg->pdr_records[data->pdr_record_count].handle = record_handle;
	cfg->pdr_records[data->pdr_record_count].offset = data->pdr_repo_size;
	cfg->pdr_records[data->pdr_record_count].length = record_len;
	data->pdr_record_count++;
	data->pdr_repo_size += record_len;
	data->pdr_largest_record_size = MAX(data->pdr_largest_record_size, record_len);

	return 0;
}

int pldm_pdr_get_repository_info(const struct device *pdr_dev, struct pldm_pdr_repo_info *repo_info)
{
	const struct pldm_pdr_provider_data *data;

	if (pdr_dev == NULL || repo_info == NULL) {
		return -EINVAL;
	}

	if (!device_is_ready(pdr_dev)) {
		return -ENODEV;
	}

	data = pdr_dev->data;
	repo_info->record_count = data->pdr_record_count;
	repo_info->repo_size = data->pdr_repo_size;
	repo_info->largest_record_size = data->pdr_largest_record_size;

	return 0;
}

int pldm_pdr_find_record_index(const struct device *pdr_dev, uint32_t record_handle,
			       uint32_t *record_index)
{
	const struct pldm_pdr_provider_config *cfg;
	const struct pldm_pdr_provider_data *data;

	if (pdr_dev == NULL || record_index == NULL) {
		return -EINVAL;
	}

	if (!device_is_ready(pdr_dev)) {
		return -ENODEV;
	}

	cfg = pdr_dev->config;
	data = pdr_dev->data;

	for (uint32_t i = 0U; i < data->pdr_record_count; i++) {
		if (cfg->pdr_records[i].handle == record_handle) {
			*record_index = i;
			return 0;
		}
	}

	return -ENOENT;
}

int pldm_pdr_get_record_at_index(const struct device *pdr_dev, uint32_t record_index,
				 struct pldm_pdr_record_metadata *record_meta)
{
	const struct pldm_pdr_provider_config *cfg;
	const struct pldm_pdr_provider_data *data;
	const struct pldm_pdr_record_info *record_info;

	if (pdr_dev == NULL || record_meta == NULL) {
		return -EINVAL;
	}

	if (!device_is_ready(pdr_dev)) {
		return -ENODEV;
	}

	cfg = pdr_dev->config;
	data = pdr_dev->data;
	if (record_index >= data->pdr_record_count) {
		return -ERANGE;
	}

	record_info = &cfg->pdr_records[record_index];
	if (record_info->offset > cfg->pdr_repo_capacity ||
	    record_info->length > (cfg->pdr_repo_capacity - record_info->offset)) {
		return -EINVAL;
	}

	record_meta->record_handle = record_info->handle;
	record_meta->record_data = &cfg->pdr_repo[record_info->offset];
	record_meta->record_len = record_info->length;
	record_meta->next_record_handle = ((record_index + 1U) < data->pdr_record_count)
						  ? cfg->pdr_records[record_index + 1U].handle
						  : 0U;

	return 0;
}

const struct pldm_pdr_numeric_sensor_desc *
pldm_pdr_find_numeric_sensor_by_id(const struct device *pdr_dev, uint16_t sensor_id)
{
	const struct pldm_pdr_provider_config *cfg;

	if (pdr_dev == NULL || !device_is_ready(pdr_dev)) {
		return NULL;
	}

	cfg = pdr_dev->config;
	for (size_t i = 0U; i < cfg->sensor_count; i++) {
		if (cfg->sensors[i].sensor_id == sensor_id) {
			return &cfg->sensors[i];
		}
	}

	return NULL;
}

static int pldm_pdr_build_chunk(const struct device *pdr_dev, uint32_t record_index, size_t offset,
				const struct pldm_pdr_get_chunk_request *req,
				struct pldm_pdr_get_chunk_response *resp)
{
	struct pldm_pdr_record_metadata record_meta;
	bool is_first;
	bool is_last;
	int rc;

	rc = pldm_pdr_get_record_at_index(pdr_dev, record_index, &record_meta);
	if (rc != 0) {
		return rc;
	}
	if (offset >= record_meta.record_len) {
		return -ERANGE;
	}

	resp->next_record_handle = record_meta.next_record_handle;

	resp->response_count = MIN((size_t)req->request_count, record_meta.record_len - offset);
	resp->response_count = MIN(resp->response_count, req->max_response_count);
	resp->record_data = &record_meta.record_data[offset];

	is_first = (offset == 0U);
	is_last = ((offset + resp->response_count) >= record_meta.record_len);

	if (is_first && is_last) {
		resp->transfer_flag = PLDM_START_AND_END;
	} else if (is_first) {
		resp->transfer_flag = PLDM_START;
	} else if (is_last) {
		resp->transfer_flag = PLDM_END;
	} else {
		resp->transfer_flag = PLDM_MIDDLE;
	}

	resp->next_data_transfer_handle = is_last ? 0U : (uint32_t)(offset + resp->response_count);
	{
		struct pldm_pdr_provider_data *data = pdr_dev->data;

		data->transfer_active = !is_last;
		data->transfer_record_handle = is_last ? 0U : record_meta.record_handle;
		data->transfer_record_index = is_last ? 0U : record_index;
		data->transfer_next_data_handle = resp->next_data_transfer_handle;
	}

	resp->transfer_crc =
		is_last ? pldm_edac_crc8(record_meta.record_data, record_meta.record_len) : 0U;

	return 0;
}

int pldm_pdr_get_chunk(const struct device *pdr_dev, const struct pldm_pdr_get_chunk_request *req,
		       struct pldm_pdr_get_chunk_response *resp)
{
	uint32_t record_index;
	int rc;

	if (pdr_dev == NULL || req == NULL || resp == NULL || req->request_count == 0U ||
	    req->max_response_count == 0U) {
		return -EINVAL;
	}

	if (!device_is_ready(pdr_dev)) {
		return -ENODEV;
	}

	if (req->transfer_op_flag == PLDM_GET_FIRSTPART) {
		if (req->record_handle == 0U) {
			record_index = 0U;
		} else {
			rc = pldm_pdr_find_record_index(pdr_dev, req->record_handle, &record_index);
			if (rc != 0) {
				return rc;
			}
		}

		return pldm_pdr_build_chunk(pdr_dev, record_index, 0U, req, resp);
	}

	if (req->transfer_op_flag == PLDM_GET_NEXTPART) {
		const struct pldm_pdr_provider_data *data = pdr_dev->data;

		if (!data->transfer_active || req->record_handle != data->transfer_record_handle ||
		    req->data_transfer_handle != data->transfer_next_data_handle) {
			return -EBADMSG;
		}

		return pldm_pdr_build_chunk(pdr_dev, data->transfer_record_index,
					    req->data_transfer_handle, req, resp);
	}

	return -EOPNOTSUPP;
}

static int pldm_pdr_provider_init(const struct device *dev)
{
	const struct pldm_pdr_provider_config *cfg = dev->config;
	struct pldm_pdr_provider_data *data = dev->data;
	struct pldm_terminus_locator_pdr tl = {0};
	uint32_t record_handle = PLDM_PDR_RECORD_HANDLE;
	int rc;

	tl.hdr.record_handle = sys_cpu_to_le32(record_handle);
	tl.hdr.version = 1U;
	tl.hdr.type = PLDM_TERMINUS_LOCATOR_PDR;
	tl.hdr.record_change_num = sys_cpu_to_le16(PLDM_PDR_RECORD_CHANGE_NUM);
	tl.hdr.length = sys_cpu_to_le16(sizeof(tl) - sizeof(struct pldm_pdr_hdr));
	tl.terminus_handle = sys_cpu_to_le16(PLDM_PDR_TERMINUS_HANDLE);
	tl.validity = PLDM_TL_PDR_VALID;
	tl.tid = 0U;
	tl.container_id = 0U;
	tl.terminus_locator_type = PLDM_TERMINUS_LOCATOR_TYPE_MCTP_EID;
	tl.terminus_locator_value_size = 1U;
	tl.terminus_locator_value[0] = 0U;

	rc = pldm_pdr_append_record(cfg, data, record_handle, &tl, sizeof(tl));
	if (rc != 0) {
		return rc;
	}

	for (size_t i = 0; i < cfg->sensor_count; i++) {
		struct pldm_numeric_sensor_value_pdr pdr = {0};
		uint8_t pdr_record[PLDM_PDR_NUMERIC_SENSOR_RECORD_WIRE_SIZE];
		size_t pdr_record_len;
		const struct pldm_pdr_numeric_sensor_desc *sensor = &cfg->sensors[i];
		const bool has_aux_name =
			(sensor->aux_name != NULL) && (sensor->aux_name[0] != '\0');

		record_handle++;
		pdr.hdr.record_handle = record_handle;
		pdr.hdr.version = 1U;
		pdr.hdr.type = PLDM_NUMERIC_SENSOR_PDR;
		pdr.hdr.record_change_num = PLDM_PDR_RECORD_CHANGE_NUM;
		pdr.hdr.length =
			PLDM_PDR_NUMERIC_SENSOR_RECORD_WIRE_SIZE - sizeof(struct pldm_pdr_hdr);
		pdr.terminus_handle = PLDM_PDR_TERMINUS_HANDLE;
		pdr.sensor_id = sensor->sensor_id;
		pdr.entity_type = sensor->entity_type;
		pdr.entity_instance = sensor->entity_instance;
		pdr.container_id = sensor->container_id;
		pdr.sensor_init = PLDM_NO_INIT;
		pdr.sensor_auxiliary_names_pdr = has_aux_name;
		pdr.base_unit = sensor->base_unit;
		pdr.unit_modifier = sensor->unit_modifier;
		pdr.rate_unit = PLDM_RATE_UNIT_NONE;
		pdr.base_oem_unit_handle = 0U;
		pdr.aux_unit = PLDM_SENSOR_UNIT_NONE;
		pdr.aux_unit_modifier = 0;
		pdr.aux_rate_unit = PLDM_RATE_UNIT_NONE;
		pdr.rel = 0U;
		pdr.aux_oem_unit_handle = 0U;
		pdr.is_linear = true;
		pdr.sensor_data_size = PLDM_SENSOR_DATA_SIZE_SINT32;
		pdr.resolution = 1.0f;
		pdr.offset = 0.0f;
		pdr.accuracy = 0U;
		pdr.plus_tolerance = 0U;
		pdr.minus_tolerance = 0U;
		pdr.hysteresis.value_s32 = 0;
		pdr.supported_thresholds.byte = 0U;
		pdr.threshold_and_hysteresis_volatility.byte = 0U;
		pdr.state_transition_interval = 0.0f;
		pdr.update_interval = 0.0f;
		pdr.max_readable.value_s32 = INT32_MAX;
		pdr.min_readable.value_s32 = INT32_MIN;
		pdr.range_field_format = PLDM_RANGE_FIELD_FORMAT_SINT32;
		pdr.range_field_support.byte = 0U;
		pdr.nominal_value.value_s32 = 0;
		pdr.normal_max.value_s32 = 0;
		pdr.normal_min.value_s32 = 0;
		pdr.warning_high.value_s32 = 0;
		pdr.warning_low.value_s32 = 0;
		pdr.critical_high.value_s32 = 0;
		pdr.critical_low.value_s32 = 0;
		pdr.fatal_high.value_s32 = 0;
		pdr.fatal_low.value_s32 = 0;

		rc = pldm_pdr_encode_numeric_sensor_record(&pdr, pdr_record, sizeof(pdr_record),
							   &pdr_record_len);
		if (rc != 0) {
			return rc;
		}
		if (pdr_record_len != PLDM_PDR_NUMERIC_SENSOR_RECORD_WIRE_SIZE) {
			return -EOVERFLOW;
		}

		rc = pldm_pdr_append_record(cfg, data, record_handle, pdr_record, pdr_record_len);
		if (rc != 0) {
			return rc;
		}

		if (has_aux_name) {
			record_handle++;
			rc = pldm_pdr_append_sensor_aux_name_record(
				cfg, data, record_handle, sensor->sensor_id, sensor->aux_name);
			if (rc != 0) {
				return rc;
			}
		}
	}

	return 0;
}

/* clang-format off */
#define PLDM_PDR_SENSOR_DESC_ENTRY(node_id)                                                        \
	{                                                                                          \
		.sensor = DEVICE_DT_GET(DT_PHANDLE(node_id, sensor)),                              \
		.channel_type = DT_PROP(node_id, channel_type),                                    \
		.channel_index = DT_PROP(node_id, channel_index),                                  \
		.sensor_id = DT_PROP(node_id, sensor_id),                                          \
		.entity_type = DT_PROP_OR(node_id, entity_type, 135),                              \
		.entity_instance = DT_PROP_OR(node_id, entity_instance, 1),                        \
		.container_id = DT_PROP_OR(node_id, container_id, 0),                              \
		.base_unit = COND_CODE_1(DT_NODE_HAS_PROP(node_id, base_unit),                     \
					(DT_PROP(node_id, base_unit)),                             \
					(PLDM_PDR_BASE_UNIT_FROM_CHANNEL_TYPE(                     \
						DT_PROP(node_id, channel_type)))),                 \
			 .unit_modifier = DT_PROP(node_id, unit_modifier),                         \
			 .aux_name = COND_CODE_1(DT_NODE_HAS_PROP(node_id, sensor_name),           \
				       (DT_PROP(node_id, sensor_name)), (NULL)),                   \
			 },

#define PLDM_PDR_SENSOR_DESC_IF(node_id)                                                           \
	COND_CODE_1(DT_NODE_HAS_COMPAT(node_id, tenstorrent_pldm_numeric_sensor),                  \
			(PLDM_PDR_SENSOR_DESC_ENTRY(node_id)), ())

#define PLDM_PDR_SENSOR_COUNT_IF(node_id)                                                          \
	+COND_CODE_1(DT_NODE_HAS_COMPAT(node_id, tenstorrent_pldm_numeric_sensor), (1), (0))

#define PLDM_PDR_SENSOR_COUNT(inst)                                                                \
	(0 DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), PLDM_PDR_SENSOR_COUNT_IF))

#define PLDM_PDR_SENSOR_AUX_COUNT_IF(node_id)                                                      \
	+COND_CODE_1(                                                                              \
		DT_NODE_HAS_COMPAT(node_id, tenstorrent_pldm_numeric_sensor),                      \
		(COND_CODE_1(DT_NODE_HAS_PROP(node_id, sensor_name), (1), (0))),                   \
		(0))

#define PLDM_PDR_AUX_COUNT(inst)                                                                   \
	(0 DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), PLDM_PDR_SENSOR_AUX_COUNT_IF))

#define PLDM_PDR_RECORD_COUNT(inst) (1U + PLDM_PDR_SENSOR_COUNT(inst) + PLDM_PDR_AUX_COUNT(inst))

#define PLDM_PDR_SENSOR_NUMERIC_SIZE_IF(node_id)                                                   \
	+COND_CODE_1(DT_NODE_HAS_COMPAT(node_id, tenstorrent_pldm_numeric_sensor),                 \
		      (PLDM_PDR_NUMERIC_SENSOR_RECORD_WIRE_SIZE), (0U))

#define PLDM_PDR_AUX_NAME_RECORD_SIZE(name_literal)                                                \
	((sizeof(struct pldm_sensor_auxiliary_names_pdr) - 1U) +                                   \
	 (1U + sizeof(PLDM_PDR_SENSOR_AUX_LANG) + (2U * sizeof(name_literal))))

#define PLDM_PDR_SENSOR_AUX_SIZE_IF(node_id)                                                       \
	+COND_CODE_1(                                                                              \
		DT_NODE_HAS_COMPAT(node_id, tenstorrent_pldm_numeric_sensor),                      \
		(COND_CODE_1(DT_NODE_HAS_PROP(node_id, sensor_name),                               \
			     (PLDM_PDR_AUX_NAME_RECORD_SIZE(DT_PROP(node_id, sensor_name))),       \
			     (0U))),                                                               \
		(0U))

#define PLDM_PDR_NUMERIC_RECORD_BYTES(inst)                                                        \
	(0 DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), PLDM_PDR_SENSOR_NUMERIC_SIZE_IF))

#define PLDM_PDR_AUX_RECORD_BYTES(inst)                                                            \
	(0 DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), PLDM_PDR_SENSOR_AUX_SIZE_IF))

#define PLDM_PDR_REPO_SIZE(inst)                                                                   \
	(sizeof(struct pldm_terminus_locator_pdr) + PLDM_PDR_NUMERIC_RECORD_BYTES(inst) +          \
	 PLDM_PDR_AUX_RECORD_BYTES(inst))

#define PLDM_PDR_PROVIDER_DEFINE(inst)                                                             \
	static const struct pldm_pdr_numeric_sensor_desc                                           \
		pldm_pdr_sensor_desc_##inst[MAX(1, PLDM_PDR_SENSOR_COUNT(inst))] = {               \
			DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), PLDM_PDR_SENSOR_DESC_IF)}; \
	static struct pldm_pdr_record_info pldm_pdr_records_##inst[PLDM_PDR_RECORD_COUNT(inst)];   \
	static uint8_t pldm_pdr_repo_##inst[PLDM_PDR_REPO_SIZE(inst)];                             \
	static struct pldm_pdr_provider_data pldm_pdr_data_##inst;                                 \
	static const struct pldm_pdr_provider_config pldm_pdr_cfg_##inst = {                       \
		.sensors = pldm_pdr_sensor_desc_##inst,                                            \
		.sensor_count = PLDM_PDR_SENSOR_COUNT(inst),                                       \
		.pdr_records = pldm_pdr_records_##inst,                                            \
		.pdr_record_capacity = ARRAY_SIZE(pldm_pdr_records_##inst),                        \
		.pdr_repo = pldm_pdr_repo_##inst,                                                  \
		.pdr_repo_capacity = sizeof(pldm_pdr_repo_##inst),                                 \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, pldm_pdr_provider_init, NULL, &pldm_pdr_data_##inst,           \
			      &pldm_pdr_cfg_##inst,                                                \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL)

/* clang-format on */

DT_INST_FOREACH_STATUS_OKAY(PLDM_PDR_PROVIDER_DEFINE)
