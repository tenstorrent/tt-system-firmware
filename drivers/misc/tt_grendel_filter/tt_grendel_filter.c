/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/drivers/misc/tt_grendel_filter.h>

#ifdef CONFIG_LOG
#include <zephyr/logging/log.h>
#else
#define LOG_ERR(...)
#define LOG_INF(...)
#define LOG_MODULE_REGISTER(...)
#endif

#include <err.h>
#include <firewall.h>

#ifndef CONFIG_TT_GRENDEL_FILTER_LOG_LEVEL
#define CONFIG_TT_GRENDEL_FILTER_LOG_LEVEL 0
#endif

LOG_MODULE_REGISTER(tt_grendel_filter, CONFIG_TT_GRENDEL_FILTER_LOG_LEVEL);

#define FILTER_ADDRESS_MASK FIREWALL_ADDR_MASK
#define FILTER_MAX_ENTRIES  16U

int tt_grendel_filter_enable(uint32_t bank, uint32_t entry, uint32_t hsio_tile, uint64_t start,
			     uint64_t end, struct tt_grendel_filter_flags flags)
{
	filter_fields_t fields = {0};
	grendel_err_t err;

	if (bank >= LAST_FILTER_TYPE || entry >= FILTER_MAX_ENTRIES || start > end ||
	    (start & ~FILTER_ADDRESS_MASK) != 0 || (end & ~FILTER_ADDRESS_MASK) != 0 ||
	    (flags.read == 0 && flags.write == 0)) {
		return -EINVAL;
	}

	fields.read_en = flags.read;
	fields.write_en = flags.write;
	fields.addr_mode = 1;
	fields.allow_ns = flags.nonsecure;
	fields.allow_burst = flags.burst;

	err = filter_arm((filter_t)bank, entry, hsio_tile, start, end, fields);
	switch (err) {
	case GRENDEL_ERR_OK:
		return 0;
	case GRENDEL_ERR_FIREWALL_INVALID_PARAM:
		return -EINVAL;
	default:
		return -EIO;
	}
}

int tt_grendel_filter_disable(uint32_t bank, uint32_t entry, uint32_t hsio_tile)
{
	grendel_err_t err;

	if (bank >= LAST_FILTER_TYPE || entry >= FILTER_MAX_ENTRIES) {
		return -EINVAL;
	}

	err = filter_disable((filter_t)bank, entry, hsio_tile);
	switch (err) {
	case GRENDEL_ERR_OK:
		return 0;
	case GRENDEL_ERR_FIREWALL_INVALID_PARAM:
		return -EINVAL;
	default:
		return -EIO;
	}
}
