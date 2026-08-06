/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_RESET_RESET_TT_BH_H_
#define ZEPHYR_INCLUDE_DRIVERS_RESET_RESET_TT_BH_H_

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Byte stride between consecutive banks in a multi-bank BH reset region. */
#define TT_BH_RESET_BANK_STRIDE 4U

/**
 * @brief Number of 32-bit banks for a BH reset DT node.
 *
 * Prefers @p n-banks (emul / non-MMIO). Otherwise derives from @p reg size /
 * #TT_BH_RESET_BANK_STRIDE (HW).
 */
#define TT_BH_RESET_NUM_BANKS(node_id)                                                             \
	COND_CODE_1(DT_NODE_HAS_PROP(node_id, n_banks), (DT_PROP(node_id, n_banks)),               \
		    (DT_REG_SIZE(node_id) / TT_BH_RESET_BANK_STRIDE))

/**
 * @brief Absolute write to a BH reset-unit bank register.
 *
 * Sets the entire bank word at offset 0 (no read-modify-write). Use when init
 * must force a known full-register state (e.g. dual-field tile half deasserted
 * with RISC half still asserted). Prefer reset_tt_bh_lines_assert/deassert when
 * only selected lines should change.
 */
int reset_tt_bh_write(const struct device *dev, uint32_t value);

/**
 * @brief Assert lines in @p mask at @p offset with a single RMW.
 *
 * @p offset is a byte offset from the controller base (typically a multiple of
 * #TT_BH_RESET_BANK_STRIDE). Active-low: clears bits in @p mask. Invalid if
 * @p offset is out of range or @p mask shares no bits with the controller's
 * reset-mask.
 */
int reset_tt_bh_lines_assert(const struct device *dev, uint32_t offset, uint32_t mask);

/**
 * @brief Deassert lines in @p mask at @p offset with a single RMW.
 *
 * Active-low: sets bits in @p mask. Used for dual-field banks (eth/ddr) where
 * tile and RISC halves share one MMIO word, multi-bank tensix regions, and
 * full-bank releases.
 */
int reset_tt_bh_lines_deassert(const struct device *dev, uint32_t offset, uint32_t mask);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_RESET_RESET_TT_BH_H_ */
