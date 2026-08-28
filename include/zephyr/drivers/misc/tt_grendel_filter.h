/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_TT_GRENDEL_FILTER_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_TT_GRENDEL_FILTER_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tt_grendel_filter_flags {
	uint32_t read: 1;
	uint32_t write: 1;
	uint32_t nonsecure: 1;
	uint32_t burst: 1;
};

/**
 * @brief Enable a filter entry for an inclusive address range.
 *
 * @param bank SiVal filter_t bank.
 * @param entry Filter entry index.
 * @param hsio_tile HSIO/SMN tile (0 for SMC banks).
 * @param start First address admitted by the entry.
 * @param end Last address admitted by the entry.
 * @param flags Accesses admitted by the entry.
 *
 * @retval 0 Entry enabled.
 * @retval -EINVAL Invalid entry, range, or flags.
 * @retval -EIO SiVal programming failed.
 */
int tt_grendel_filter_enable(uint32_t bank, uint32_t entry, uint32_t hsio_tile, uint64_t start,
			     uint64_t end, struct tt_grendel_filter_flags flags);

/**
 * @brief Disable a filter entry.
 *
 * @param bank SiVal filter_t bank.
 * @param entry Filter entry index.
 * @param hsio_tile HSIO/SMN tile (0 for SMC banks).
 *
 * @retval 0 Entry disabled.
 * @retval -EINVAL Invalid entry.
 * @retval -EIO SiVal programming failed.
 */
int tt_grendel_filter_disable(uint32_t bank, uint32_t entry, uint32_t hsio_tile);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_TT_GRENDEL_FILTER_H_ */
