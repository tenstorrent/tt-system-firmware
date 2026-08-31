/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_TT_GRENDEL_AXI_FILTER_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_TT_GRENDEL_AXI_FILTER_H_

#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tt_grendel_axi_filter_flags {
	uint32_t read: 1;
	uint32_t write: 1;
	uint32_t nonsecure: 1;
	uint32_t burst: 1;
};

/**
 * @brief Enable a filter entry for an inclusive address range.
 *
 * @param dev Filter bank device.
 * @param entry Filter entry index.
 * @param start First address admitted by the entry.
 * @param end Last address admitted by the entry.
 * @param flags Accesses admitted by the entry.
 *
 * @retval 0 Entry enabled.
 * @retval -EINVAL Invalid entry, range, or flags.
 */
int tt_grendel_axi_filter_enable(const struct device *dev, uint32_t entry, uint64_t start,
				 uint64_t end, struct tt_grendel_axi_filter_flags flags);

/**
 * @brief Disable a filter entry.
 *
 * @param dev Filter bank device.
 * @param entry Filter entry index.
 *
 * @retval 0 Entry disabled.
 * @retval -EINVAL Invalid entry.
 */
int tt_grendel_axi_filter_disable(const struct device *dev, uint32_t entry);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_TT_GRENDEL_AXI_FILTER_H_ */
