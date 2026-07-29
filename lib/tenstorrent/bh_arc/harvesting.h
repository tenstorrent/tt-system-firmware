/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HARVESTING_H
#define HARVESTING_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint16_t tensix_col_enabled; /* Bitmap 0-13 */
	uint16_t eth_enabled; /* Bitmap 0-13. 1 = Allowed to use, not necessarily connected outside
			       * of chip
			       */
	bool eth5_serdes;     /* False = serdes2 lane 0-3, True = serdes2 lane 4-7 */
	bool eth8_serdes;     /* False = serdes5 lane 7-4, True = serdes5 lane 3-0 */
	uint16_t eth_serdes_connected; /* Bitmap 0-11. 1 = Expect an outside board connection */
	uint8_t gddr_enabled;          /* Bitmap 0-7 */
	uint8_t l2cpu_enabled;         /* Bitmap 0-3. Shows L2CPU cluster enablement */
	uint8_t pcie_enabled;          /* Bitmap 0-1. Shows PCIe instance enablement */
	uint8_t pcie_usage[2];         /* bh_pcie_mode_t per PCIe instance (see chip_info.h) */
	uint8_t pcie_num_serdes[2];    /* 1 or 2 if enabled */
} TileEnable;

extern TileEnable tile_enable;

#ifdef __cplusplus
}
#endif

#endif
