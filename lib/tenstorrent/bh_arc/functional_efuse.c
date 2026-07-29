/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/util.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/bh_efuse.h>
#include <zephyr/logging/log.h>

#include "functional_efuse.h"

LOG_MODULE_REGISTER(functional_efuse, CONFIG_TT_APP_LOG_LEVEL);

#if DT_NODE_EXISTS(DT_NODELABEL(efuse))
static const struct device *const efuse_dev = DEVICE_DT_GET(DT_NODELABEL(efuse));
#else
static const struct device *const efuse_dev;
#endif

static uint32_t read_efuse_dword(uint32_t offset)
{
	uint32_t value = 0;
	int rc;

	rc = tt_bh_efuse_read(efuse_dev, TT_BH_EFUSE_ACCESS_DIRECT, TT_BH_EFUSE_BOX_FUNC, offset,
			      &value);
	if (rc != 0) {
		LOG_ERR("efuse read failed (offset=%u): %d", offset, rc);
		return 0;
	}

	return value;
}

/* Extracts fields from the functional efuse from start_bit to end_bit (inclusive) */
/* Note that this only works for fields that are 32-bits or smaller */
/* i.e. end_bit - start_bit < 32 */
uint32_t ReadFunctionalEfuse(uint32_t start_bit, uint32_t end_bit)
{
	int32_t field_length = end_bit - start_bit + 1;

	if (field_length > 32 || field_length < 1) {
		/* These are error cases, just return 0 */
		return 0;
	}

	uint32_t start_index = start_bit / 32;
	uint64_t data = 0;

	/* We must read 4 bytes at a time as a uint32_t */
	/* But we want to handle the case where a field spans across two dwords */
	data = read_efuse_dword(start_index);
	data |= (uint64_t)read_efuse_dword(start_index + 1) << 32;
	/* Corner case: this will read past the end of the functional efuse */
	/* when we try to access the last dword, */
	/* but that case should be safe from the HW perspective */

	/* Mask and shift the bits we want */
	uint64_t mask = GENMASK64(field_length + (start_bit % 32) - 1, start_bit % 32);

	return FIELD_GET(mask, data);
}
