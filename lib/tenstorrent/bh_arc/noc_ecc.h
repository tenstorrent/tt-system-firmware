/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NOC_ECC_H
#define NOC_ECC_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include "noc_init.h"

/*
 * Tensix, ETH, and GDDR NIUs map their registers at 0xFFB20000. NOC2AXI nodes (ARC, PCIE)
 * use a different base; counters are at NOC_NIU_ECC_COUNTERS_OFF from that base.
 * Hardware counters are 16-bit with no saturation - a link erroring continuously wraps
 * back through zero and briefly looks healthy.
 */
#define NOC_NIU_REGS_BASE          0xFFB20000ull
#define NOC_NIU1_REGS_BASE         0xFFB30000ull /* same tile, NOC 1 NIU */
#define NOC_NIU_ECC_COUNTERS_OFF   0x50
#define NOC_NIU_ECC_CTRL_OFF       0x5C
#define NOC_NIU_NUM_MEM_PARITY_ERR (NOC_NIU_REGS_BASE + NOC_NIU_ECC_COUNTERS_OFF)
#define NOC_NIU_NUM_HEADER_1B_ERR  (NOC_NIU_REGS_BASE + 0x54)
#define NOC_NIU_NUM_HEADER_2B_ERR  (NOC_NIU_REGS_BASE + 0x58)

/*
 * The NIU's own ECC_CTRL. Write-only: reads return zero, so this one must never be
 * read-modify-written. Each of the three sources has a clear bit in [2:0] and a force bit in
 * [5:3]; both are decoded off the write strobe, so one write is one pulse. NUM_MEM_PARITY_ERR
 * also adds every memory-parity error strobe that is high, so one force can move that counter
 * by more than one. Address is node-base + NOC_NIU_ECC_CTRL_OFF; the Tensix/ETH/GDDR absolute
 * is kept for the original NIU window.
 */
#define NOC_NIU_ECC_CTRL       (NOC_NIU_REGS_BASE + NOC_NIU_ECC_CTRL_OFF)
#define NOC_NIU_ECC_CTRL_CLEAR GENMASK(2, 0)
#define NOC_NIU_ECC_CTRL_FORCE GENMASK(5, 3)

/* Bit position within either 3-bit field above, and index into the counter array below. */
#define NOC_ECC_MEM_PARITY  0
#define NOC_ECC_HDR_SBE     1
#define NOC_ECC_HDR_DBE     2
#define NOC_ECC_NUM_SOURCES 3
#define NOC_ECC_SOURCE_MASK GENMASK(2, 0)

/* NOC_ECC_TLB, NOC_ECC_RING, and NocEccTlbLock/Unlock come from noc_init.h, which is
 * in the recovery library so the ROUTER_CFG_0 walks there can share the same TLB.
 */

/**
 * @brief True if (noc_x, noc_y) is a node whose NOC 0 NIU this module knows how to address.
 *
 * Tensix, ETH, GDDR, PCIE, and ARC. SERDES and other row-0 nodes, and column 8 above
 * row 0, are rejected: a NOC read to a node without a reachable NIU can hang ARC.
 * Coordinates are in whatever system ARC currently speaks; the accepted rectangle is
 * the same with translation on or off.
 */
bool NocEccIsKnownNode(uint8_t noc_x, uint8_t noc_y);

/* NIU_CFG_0 bits that matter to ECC reporting. */
#define NOC_NIU_CFG_0_OFF          0x100
#define NOC_NIU_CFG_0_ECC_IRQ_EN   GENMASK(11, 9) /* [9] mem parity, [10] hdr SBE, [11] hdr DBE */
#define NOC_NIU_CFG_0_TILE_CLK_OFF BIT(12)

/**
 * @brief Read NIU_CFG_0 through an already-programmed @ref NOC_ECC_TLB.
 *
 * Caller holds the ECC TLB lock and has pointed @ref NOC_ECC_TLB at a window that
 * covers @p niu_base.
 */
uint32_t NocNiuCfg0ReadLocked(uint64_t niu_base);

/**
 * @brief NIU_CFG_0 tile-clock-off through an already-programmed @ref NOC_ECC_TLB.
 *
 * Caller holds the ECC TLB lock and has pointed @ref NOC_ECC_TLB at a window that
 * covers @p niu_base. Lets a per-tile walk read the gate bit, the NIU counters, and
 * the Tensix ECC_STATUS view with one TLB program.
 */
bool NocNiuTileClockGatedLocked(uint64_t niu_base);

/**
 * @brief Read a node's three NOC 0 NIU ECC error counters.
 *
 * @param noc_x    NOC 0 X coordinate ARC should address (logical if translation is on).
 * @param noc_y    NOC 0 Y coordinate.
 * @param niu_base Node NIU register base. Tensix/ETH/GDDR use @ref NOC_NIU_REGS_BASE.
 * @param out      Receives the counts, indexed by NOC_ECC_MEM_PARITY / _HDR_SBE / _HDR_DBE.
 */
void NocNiuEccReadCounters(uint8_t noc_x, uint8_t noc_y, uint64_t niu_base,
			   uint32_t out[NOC_ECC_NUM_SOURCES]);

/**
 * @brief Same as @ref NocNiuEccReadCounters through an already-programmed @ref NOC_ECC_TLB.
 *
 * Caller holds the ECC TLB lock and has pointed @ref NOC_ECC_TLB at a window covering
 * @p niu_base.
 */
void NocNiuEccReadCountersLocked(uint64_t niu_base, uint32_t out[NOC_ECC_NUM_SOURCES]);

/**
 * @brief Read a NOC 0 node's three NIU ECC error counters.
 *
 * Picks the NIU window from the node type. ARC (8,0) is local MMIO so this never
 * NOC-TLBs to ARC itself.
 *
 * @param noc_x NOC 0 X coordinate of the node.
 * @param noc_y NOC 0 Y coordinate of the node.
 * @param out   Receives the counts, indexed by NOC_ECC_MEM_PARITY / _HDR_SBE / _HDR_DBE.
 */
void NocEccReadCounters(uint8_t noc_x, uint8_t noc_y, uint32_t out[NOC_ECC_NUM_SOURCES]);

/**
 * @brief Zero the selected NIU ECC counters through an already-programmed @ref NOC_ECC_TLB.
 *
 * Caller holds the ECC TLB lock and has pointed @ref NOC_ECC_TLB at a window covering
 * @p niu_base. Dropping a counter to zero also drops the NIU's error level for that source,
 * which is what re-arms the Tensix ECC manager's edge detect for the next interrupt.
 */
void NocNiuEccClearLocked(uint64_t niu_base, uint8_t which);

/**
 * @brief Read ARC's own NOC 0 NIU ECC counters via local MMIO.
 *
 * ARC must not NOC-TLB to its own coordinates.
 */
void ArcNoc0EccReadCounters(uint32_t out[NOC_ECC_NUM_SOURCES]);

/**
 * @brief Force one ECC error per selected source on a node's NOC NIU.
 *
 * The force bit is one pulse and does not depend on ECC checking being enabled.
 * @c NUM_MEM_PARITY_ERR also sums live parity-error strobes, so that counter can
 * advance by more than one. ARC is forced through local MMIO.
 *
 * @param noc_x NOC 0 X coordinate of the node.
 * @param noc_y NOC 0 Y coordinate of the node.
 * @param which Bitmask of NOC_ECC_MEM_PARITY / _HDR_SBE / _HDR_DBE bit positions.
 */
void NocEccForce(uint8_t noc_x, uint8_t noc_y, uint8_t which);

#endif /* NOC_ECC_H */
