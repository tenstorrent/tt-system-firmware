/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_RESET_TT_BH_RESET_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_RESET_TT_BH_RESET_H_

/**
 * @file
 * @brief Bit-position reset line IDs for Tenstorrent Blackhole reset banks.
 *
 * Dual-field registers share one controller instance; tile and RISC halves use
 * distinct bit ranges. IDs are physical bit indices into the MMIO word.
 */

/* eth_reset @ 0x80030008: tile [13:0], RISC [29:16] */
#define TT_BH_ETH_TILE_ID(n) ((n) + 0)
#define TT_BH_ETH_RISC_ID(n) ((n) + 16)

/* ddr_reset @ 0x80030010: tile [7:0], RISC [31:8] */
#define TT_BH_DDR_TILE_ID(n) ((n) + 0)
#define TT_BH_DDR_RISC_ID(n) ((n) + 8)

/* l2cpu_reset @ 0x80030014: tile [3:0], RISC [7:4] */
#define TT_BH_L2CPU_TILE_ID(n) ((n) + 0)
#define TT_BH_L2CPU_RISC_ID(n) ((n) + 4)

/* global_reset @ 0x80030000 bit positions (reset-mask 0x2383) */
#define TT_BH_GLOBAL_SYSTEM_RESET_ID  0
#define TT_BH_GLOBAL_NOC_RESET_ID     1
#define TT_BH_GLOBAL_REFCLK_CNT_EN_ID 7
#define TT_BH_GLOBAL_PCIE0_RESET_ID   8
#define TT_BH_GLOBAL_PCIE1_RESET_ID   9
#define TT_BH_GLOBAL_PTP_RESET_ID     13

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_RESET_TT_BH_RESET_H_ */
