/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TENSIX_ECC_H
#define TENSIX_ECC_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include "noc_ecc.h"

/* Per-Tensix RISCV debug registers. One instance per tile, shared by all five RISC cores. */
#define TENSIX_ECC_CTRL   0xFFB121D0
#define TENSIX_ECC_STATUS 0xFFB121D4

/*
 * Tensix config word 3 holds the L1 ECC scrubber fields (tt-metal blackhole cfg_defines.h:
 * ECC_SCRUBBER_*_ADDR32 == 3) alongside RISC_DEST_ACCESS_CTRL_SEC* (bits 14+), which kernels
 * write at runtime. The cfg space is not reachable over NOC2AXI (a read hangs the ARC NIU),
 * so it is modified with RMWCIB instructions injected into the tile, and only while no
 * kernel is running there.
 */
#define TENSIX_CFG_ECC_SCRUBBER_WORD 3

#define TENSIX_ECC_SCRUBBER_ENABLE                   BIT(0)
#define TENSIX_ECC_SCRUBBER_SCRUB_ON_ERROR           BIT(1)
#define TENSIX_ECC_SCRUBBER_SCRUB_ON_ERROR_IMMEDIATE BIT(2)
#define TENSIX_ECC_SCRUBBER_DELAY                    GENMASK(13, 3)

/* Same settings tt-metal's BRISC firmware applies in device_setup(). */
#define TENSIX_ECC_SCRUBBER_DELAY_DEFAULT 0x100

/*
 * ECC_CTRL layout (tt_ecc_manager.sv). [3:0] selects the ECC_STATUS view. [4] zeroes every
 * counter and [5] drops the tile's error interrupt; both are level-sensitive, so each is a
 * set-then-clear pulse. [31:8] are per-source interrupt enables; the ones firmware arms are
 * below. The interrupt is a latch set on a new error and held until [5] is pulsed. The three
 * NOC sources are edge-detected from the NIU's level (NIU_CFG_0 int-enable AND counter != 0),
 * so a NOC source fires once and not again until its NIU counter is cleared.
 */
#define TENSIX_ECC_CTRL_STATUS_CLEAR      BIT(4)
#define TENSIX_ECC_CTRL_IRQ_CLEAR         BIT(5)
#define TENSIX_ECC_CTRL_IRQ_EN_L1_SBE     BIT(8)
#define TENSIX_ECC_CTRL_IRQ_EN_L1_DBE     BIT(9)
#define TENSIX_ECC_CTRL_IRQ_EVERY_ERROR   BIT(20)
#define TENSIX_ECC_CTRL_IRQ_EN_NOC_PARITY BIT(21)
#define TENSIX_ECC_CTRL_IRQ_EN_NOC_SBE    BIT(22)
#define TENSIX_ECC_CTRL_IRQ_EN_NOC_DBE    BIT(23)

/*
 * What firmware arms: every L1 and NOC source, so the interrupt is the only collection
 * path and nothing is polled. A weak cell or marginal link that emits SBEs continuously
 * is rate-limited by the monitor's per-vector re-enable hold-off, not by leaving SBE
 * unarmed. [20] (IRQ on every error, not just a new address) stays clear.
 */
#define TENSIX_ECC_CTRL_IRQ_ARMED                                                                  \
	(TENSIX_ECC_CTRL_IRQ_EN_L1_SBE | TENSIX_ECC_CTRL_IRQ_EN_L1_DBE |                           \
	 TENSIX_ECC_CTRL_IRQ_EN_NOC_PARITY | TENSIX_ECC_CTRL_IRQ_EN_NOC_SBE |                      \
	 TENSIX_ECC_CTRL_IRQ_EN_NOC_DBE)

/*
 * ECC_STATUS is a windowed register: ECC_CTRL[3:0] selects which view it returns.
 * Any select other than 1-6 returns TENSIX_ECC_LIVENESS_MAGIC.
 */
#define TENSIX_ECC_SEL_L1_CNT   2U   /* L1 SBE/DBE counters */
#define TENSIX_ECC_SEL_NOC      4U   /* NIU error levels as the manager sees them */
#define TENSIX_ECC_SEL_LIVENESS 0xFU /* not a view - returns the magic value */

/* sel=4: [0] mem parity, [1] hdr SBE, [2] hdr DBE - same order as NOC_ECC_* indices. */
#define TENSIX_ECC_NOC_LEVEL_MASK GENMASK(2, 0)

#define TENSIX_ECC_LIVENESS_MAGIC 0xDEADBEEFU

/* sel=2. Widths are Blackhole-specific: SBE 8 bits, DBE 6 bits.
 * The overflow bit is sticky and records one wrap; the counter keeps going.
 */
#define TENSIX_ECC_L1_DBE_CNT(s) FIELD_GET(GENMASK(5, 0), (s))
#define TENSIX_ECC_L1_DBE_OVF(s) FIELD_GET(BIT(6), (s))
#define TENSIX_ECC_L1_DBE_MOD    (1U << 6)
#define TENSIX_ECC_L1_SBE_CNT(s) FIELD_GET(GENMASK(14, 7), (s))
#define TENSIX_ECC_L1_SBE_OVF(s) FIELD_GET(BIT(15), (s))
#define TENSIX_ECC_L1_SBE_MOD    (1U << 8)

/** @brief One Tensix tile's ECC counters, as collected by a service pass. */
struct tensix_ecc_tile {
	/** NOC 0 NIU ECC counters, indexed by NOC_ECC_MEM_PARITY / _HDR_SBE / _HDR_DBE. */
	uint32_t noc[NOC_ECC_NUM_SOURCES];
	/** NIU_CFG_0 tile-clock-off as read from hardware. */
	bool clock_gated;
	/** true if the tile was clocked, passed liveness, and @c l1_sbe / @c l1_dbe are valid. */
	bool l1_valid;
	uint32_t l1_sbe;
	uint32_t l1_dbe;
};

/**
 * @brief Turn on the L1 ECC background scrubber in one tile or every Tensix tile.
 *
 * Injects RMWCIB0/RMWCIB1 so the tile sets config word 3 to Enable=1, Scrub_On_Error=1,
 * Delay=@ref TENSIX_ECC_SCRUBBER_DELAY_DEFAULT (the value tt-metal's device_setup() writes)
 * while leaving bits 14 and up, which belong to RISC_DEST_ACCESS_CTRL, unchanged.
 *
 * Call only when the tile's RISCs are idle (after reset, before firmware runs), and after
 * the dummy UNPACR that absorbs the post-reset first-instruction corruption. L1 must
 * already hold valid ECC: scrubbing uninitialised L1 would saturate the sticky SBE/DBE
 * counters. Uses the unlocked TLB 0 via tensix_inject_instruction, like the UNPACR.
 * No-op when feature_enable.ecc_en is clear.
 *
 * @param broadcast true to hit every non-harvested Tensix via the multicast TLB.
 * @param noc_x     X coordinate when @p broadcast is false, in the active coordinate system.
 * @param noc_y     Y coordinate when @p broadcast is false.
 */
void TensixEccEnableScrubber(bool broadcast, uint8_t noc_x, uint8_t noc_y);

/**
 * @brief Arm the tile error interrupt for @ref TENSIX_ECC_CTRL_IRQ_ARMED in one or every tile.
 *
 * Full write of ECC_CTRL: firmware owns the register (the service path preserves [31:4]
 * on its select writes). Tile reset zeroes ECC_CTRL, so call this wherever the tile is
 * reinitialised. The write is dropped by a clock-gated tile; the Tensix power-on path arms
 * again. Takes the ECC TLB lock so a concurrent service cannot have its view select
 * overwritten mid-read. No-op when feature_enable.ecc_en is clear.
 *
 * @param broadcast true to hit every non-harvested Tensix via the multicast TLB.
 * @param noc_x     X coordinate when @p broadcast is false, in the active coordinate system.
 * @param noc_y     Y coordinate when @p broadcast is false.
 */
void TensixEccArmIrq(bool broadcast, uint8_t noc_x, uint8_t noc_y);

/**
 * @brief Collect and clear one tile's ECC state after its error interrupt.
 *
 * One TLB program, under the ECC TLB lock. Reads the NIU counters and zeroes them (which
 * drops the NIU error level so the next NOC error is a new edge). If the tile is clocked
 * and live, reads the L1 counters, zeroes them with ECC_CTRL[4], pulses ECC_CTRL[5] to drop
 * the interrupt, and leaves the register armed. A NOC error landing between the NIU clear
 * and the end of that pulse raises no edge, so the NIU counters are re-read afterwards and
 * the clear repeated while they are non-zero. All counts in @p out are deltas since the
 * last visit and must be added into running totals by the caller.
 *
 * Caller holds @ref NocEccStateLock so the gate cannot change between the gate read and
 * the L1 access.
 *
 * @param noc_x NOC 0 X coordinate of the tile.
 * @param noc_y NOC 0 Y coordinate of the tile.
 * @param out   Filled in; @c l1_valid tells whether the L1 fields mean anything.
 *
 * @return true if the tile's interrupt was cleared (tile was clocked). false means the
 *         tile is clock-gated: its interrupt, if asserted, stays asserted and cannot be
 *         cleared until the tile is ungated.
 */
bool TensixEccServiceTile(uint8_t noc_x, uint8_t noc_y, struct tensix_ecc_tile *out);

/** @brief Read-back of everything between a NOC error and the tile's error line. */
struct tensix_ecc_probe {
	/** NIU_CFG_0 of the NOC 0 and NOC 1 NIUs: [11:9] ECC int enable, [12] tile clock off. */
	uint32_t niu_cfg_0[2];
	/** NOC 0 NIU ECC counters, not cleared. */
	uint32_t noc[NOC_ECC_NUM_SOURCES];
	/** NIU_CFG_0[12] of the NOC 0 NIU; when set the two fields below were not read. */
	bool clock_gated;
	/** ECC_CTRL as the tile holds it. Compare against TENSIX_ECC_CTRL_IRQ_ARMED. */
	uint32_t ecc_ctrl;
	/** ECC_STATUS sel 4: the NIU error levels the ECC manager is edge-detecting. */
	uint32_t noc_level;
};

/**
 * @brief Read, without clearing, one Tensix tile's ECC interrupt plumbing.
 *
 * Diagnostic for the shell. Leaves counters, enables and the interrupt latch as they
 * were; only ECC_CTRL[3:0] is cycled to take the sel 4 view and then restored.
 *
 * @param noc_x NOC 0 X coordinate of the tile.
 * @param noc_y NOC 0 Y coordinate of the tile.
 * @param out   Filled in.
 */
void TensixEccProbe(uint8_t noc_x, uint8_t noc_y, struct tensix_ecc_probe *out);

#endif /* TENSIX_ECC_H */
