/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PMCI_PLDM_PDR_H_
#define PMCI_PLDM_PDR_H_

#include <zephyr/device.h>

#include <zephyr/sys/util.h>

#include <libpldm/base.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file
 * @brief PLDM PDR provider API.
 */

/** @addtogroup drivers
 *  @{
 */

/** @defgroup PLDM PLDM
 *  @brief Platform Level Data Model driver interfaces.
 *  @{
 */

/* PDR record change number used both when building records and validating GetPDR requests. */
#define PLDM_PDR_RECORD_CHANGE_NUM 0x0000U

struct pldm_pdr_get_chunk_request {
	uint32_t record_handle;
	uint32_t data_transfer_handle;
	uint8_t transfer_op_flag;
	uint16_t request_count;
	size_t max_response_count;
};

struct pldm_pdr_get_chunk_response {
	uint32_t next_record_handle;
	uint32_t next_data_transfer_handle;
	uint8_t transfer_flag;
	const uint8_t *record_data;
	size_t response_count;
	uint8_t transfer_crc;
};

struct pldm_pdr_repo_info {
	uint32_t record_count;
	size_t repo_size;
	size_t largest_record_size;
};

struct pldm_pdr_record_metadata {
	uint32_t record_handle;
	const uint8_t *record_data;
	size_t record_len;
	uint32_t next_record_handle;
};

/** Numeric sensor description sourced from devicetree. */
struct pldm_pdr_numeric_sensor_desc {
	const struct device *sensor;
	uint8_t channel_type;
	uint8_t channel_index;
	uint16_t sensor_id;
	uint16_t entity_type;
	uint16_t entity_instance;
	uint16_t container_id;
	uint8_t base_unit;
	int8_t unit_modifier;
	const char *aux_name;
};

#ifdef CONFIG_PMCI_PLDM_PLATFORM
/**
 * Update the terminus locator record with the runtime TID/EID values.
 *
 * @param pdr_dev PDR provider device instance.
 * @param tid Runtime PLDM terminus ID.
 * @param endpoint_id Runtime MCTP endpoint ID.
 * @retval 0 on success.
 * @retval -EINVAL if the repository state is invalid.
 * @retval -ENODEV if provider device is not ready.
 */
int pldm_pdr_update_tid_eid(const struct device *pdr_dev, uint8_t tid, uint8_t endpoint_id);

/**
 * Read PDR repository summary fields.
 *
 * @param pdr_dev Provider device instance.
 * @param repo_info Output repository summary.
 * @retval 0 on success.
 * @retval -EINVAL if arguments are invalid.
 * @retval -ENODEV if provider device is not ready.
 */
int pldm_pdr_get_repository_info(const struct device *pdr_dev,
				 struct pldm_pdr_repo_info *repo_info);

/**
 * Find a PDR record index by record handle.
 *
 * @param pdr_dev Provider device instance.
 * @param record_handle Input record handle.
 * @param record_index Output index in repository metadata array.
 * @retval 0 on success.
 * @retval -ENOENT if record handle does not exist.
 * @retval -EINVAL if arguments are invalid.
 * @retval -ENODEV if provider device is not ready.
 */
int pldm_pdr_find_record_index(const struct device *pdr_dev, uint32_t record_handle,
			       uint32_t *record_index);

/**
 * Get record metadata and data pointer by record index.
 *
 * @param pdr_dev Provider device instance.
 * @param record_index Input record index.
 * @param record_meta Output record metadata and data pointer.
 * @retval 0 on success.
 * @retval -EINVAL if arguments are invalid or repository metadata is corrupted.
 * @retval -ERANGE if index is out of range.
 * @retval -ENODEV if provider device is not ready.
 */
int pldm_pdr_get_record_at_index(const struct device *pdr_dev, uint32_t record_index,
				 struct pldm_pdr_record_metadata *record_meta);

/**
 * Find a numeric sensor descriptor by sensor ID.
 *
 * @param pdr_dev Provider device instance.
 * @param sensor_id Input sensor ID.
 * @return Descriptor pointer on success, NULL if not found or provider unavailable.
 */
const struct pldm_pdr_numeric_sensor_desc *
pldm_pdr_find_numeric_sensor_by_id(const struct device *pdr_dev, uint16_t sensor_id);

/**
 * Build one GetPDR response chunk and update transfer state.
 *
 * @param pdr_dev Provider device instance.
 * @param req GetPDR chunk request parameters.
 * @param resp Output chunk metadata and data pointer.
 * @retval 0 on success.
 * @retval -ENOENT if FIRSTPART record handle does not exist.
 * @retval -EBADMSG if NEXTPART transfer state/handles are invalid.
 * @retval -EOPNOTSUPP if transfer operation flag is unsupported.
 * @retval -EINVAL if arguments are invalid.
 * @retval -ENODEV if provider device is not ready.
 */
int pldm_pdr_get_chunk(const struct device *pdr_dev, const struct pldm_pdr_get_chunk_request *req,
		       struct pldm_pdr_get_chunk_response *resp);
#else

static inline int pldm_pdr_update_tid_eid(const struct device *pdr_dev, uint8_t tid,
					  uint8_t endpoint_id)
{
	ARG_UNUSED(pdr_dev);
	ARG_UNUSED(tid);
	ARG_UNUSED(endpoint_id);
	return -ENOTSUP;
}

static inline int pldm_pdr_get_repository_info(const struct device *pdr_dev,
					       struct pldm_pdr_repo_info *repo_info)
{
	ARG_UNUSED(repo_info);
	ARG_UNUSED(pdr_dev);
	return -ENOTSUP;
}

static inline int pldm_pdr_find_record_index(const struct device *pdr_dev, uint32_t record_handle,
					     uint32_t *record_index)
{
	ARG_UNUSED(record_handle);
	ARG_UNUSED(record_index);
	ARG_UNUSED(pdr_dev);
	return -ENOTSUP;
}

static inline int pldm_pdr_get_record_at_index(const struct device *pdr_dev, uint32_t record_index,
					       struct pldm_pdr_record_metadata *record_meta)
{
	ARG_UNUSED(record_index);
	ARG_UNUSED(record_meta);
	ARG_UNUSED(pdr_dev);
	return -ENOTSUP;
}

static inline const struct pldm_pdr_numeric_sensor_desc *
pldm_pdr_find_numeric_sensor_by_id(const struct device *pdr_dev, uint16_t sensor_id)
{
	ARG_UNUSED(sensor_id);
	ARG_UNUSED(pdr_dev);
	return NULL;
}

static inline int pldm_pdr_get_chunk(const struct device *pdr_dev,
				     const struct pldm_pdr_get_chunk_request *req,
				     struct pldm_pdr_get_chunk_response *resp)
{
	ARG_UNUSED(req);
	ARG_UNUSED(resp);
	ARG_UNUSED(pdr_dev);
	return -ENOTSUP;
}
#endif

/** @} */ /* PLDM */
/** @} */ /* drivers */

#endif /* PMCI_PLDM_PDR_H_ */
