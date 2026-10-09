/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tensix_ecc.h"
#include "chip_info.h"
#include "noc_ecc.h"
#include "noc2axi.h"
#include "tensix.h"

#include <zephyr/sys/util.h>

/* ECC_CTRL[3:0] view select. */
#define ECC_CTRL_SEL_MASK GENMASK(3, 0)

/*
 * ECC_CTRL[4] holds the counters in reset and [5] holds the IRQ in reset. Both are
 * level-sensitive, not write-1-to-clear: leaving [4] set makes every counter read zero
 * forever, so a faulty part looks perfectly healthy. Always mask them out of a write.
 */
#define ECC_CTRL_STATUS_CLEAR TENSIX_ECC_CTRL_STATUS_CLEAR
#define ECC_CTRL_IRQ_CLEAR    TENSIX_ECC_CTRL_IRQ_CLEAR

/* Resting value of ECC_CTRL once firmware owns it: armed, no clears, L1 counter view. */
#define ECC_CTRL_ARMED_VALUE                                                                       \
	(TENSIX_ECC_CTRL_IRQ_ARMED | FIELD_PREP(ECC_CTRL_SEL_MASK, TENSIX_ECC_SEL_L1_CNT))

/* The scrubber fields span bits [13:0] of word 3: all of byte 0 and the low 6 bits of byte 1. */
#define SCRUBBER_WORD_VALUE                                                                        \
	(TENSIX_ECC_SCRUBBER_ENABLE | TENSIX_ECC_SCRUBBER_SCRUB_ON_ERROR |                         \
	 FIELD_PREP(TENSIX_ECC_SCRUBBER_DELAY, TENSIX_ECC_SCRUBBER_DELAY_DEFAULT))
#define SCRUBBER_BYTE0_MASK 0xFFu
#define SCRUBBER_BYTE1_MASK (GENMASK(13, 8) >> 8)

BUILD_ASSERT((SCRUBBER_WORD_VALUE & ~GENMASK(13, 0)) == 0,
	     "scrubber value must stay inside the scrubber fields");

static uint32_t TensixEccReadStatusLocked(uint8_t sel)
{
	uint32_t ctrl;

	/*
	 * Read-modify-write. ECC_CTRL[31:4] holds the per-source IRQ enables, so writing the
	 * select alone would disable every ECC interrupt in this tile.
	 */
	ctrl = NOC2AXIRead32(NOC_ECC_RING, NOC_ECC_TLB, TENSIX_ECC_CTRL);
	ctrl &= ~(ECC_CTRL_SEL_MASK | ECC_CTRL_STATUS_CLEAR | ECC_CTRL_IRQ_CLEAR);
	ctrl |= FIELD_PREP(ECC_CTRL_SEL_MASK, sel);
	NOC2AXIWrite32(NOC_ECC_RING, NOC_ECC_TLB, TENSIX_ECC_CTRL, ctrl);

	return NOC2AXIRead32(NOC_ECC_RING, NOC_ECC_TLB, TENSIX_ECC_STATUS);
}

static void DecodeL1Status(uint32_t status, uint32_t *sbe, uint32_t *dbe)
{
	*sbe = (uint32_t)TENSIX_ECC_L1_SBE_CNT(status);
	if (TENSIX_ECC_L1_SBE_OVF(status)) {
		*sbe += TENSIX_ECC_L1_SBE_MOD;
	}
	*dbe = (uint32_t)TENSIX_ECC_L1_DBE_CNT(status);
	if (TENSIX_ECC_L1_DBE_OVF(status)) {
		*dbe += TENSIX_ECC_L1_DBE_MOD;
	}
}

/* Liveness, then the sel=2 view. TLB already points at this tile. */
static bool ReadL1CountersLocked(uint32_t *sbe, uint32_t *dbe)
{
	if (TensixEccReadStatusLocked(TENSIX_ECC_SEL_LIVENESS) != TENSIX_ECC_LIVENESS_MAGIC) {
		return false;
	}

	DecodeL1Status(TensixEccReadStatusLocked(TENSIX_ECC_SEL_L1_CNT), sbe, dbe);
	return true;
}

/*
 * The NOC2AXI TLB window is 2^NOC_TLB_LOG_SIZE bytes and the TLB holds addr >> 24, so
 * one program at TENSIX_ECC_CTRL also reaches the NIU block. This is what lets a tile be
 * read with one TLB write instead of three.
 */
BUILD_ASSERT((TENSIX_ECC_CTRL >> NOC_TLB_LOG_SIZE) == (NOC_NIU_REGS_BASE >> NOC_TLB_LOG_SIZE),
	     "Tensix ECC_CTRL and the NIU block must share one NOC2AXI TLB window");

void TensixEccEnableScrubber(bool broadcast, uint8_t noc_x, uint8_t noc_y)
{
	if (!bh_chip_info_feature_ecc_en()) {
		return;
	}

	const uint32_t rmw_byte0 = TENSIX_INSTRUCTION_RMWCIB(
		0, SCRUBBER_BYTE0_MASK, SCRUBBER_WORD_VALUE & 0xFFu, TENSIX_CFG_ECC_SCRUBBER_WORD);
	const uint32_t rmw_byte1 = TENSIX_INSTRUCTION_RMWCIB(1, SCRUBBER_BYTE1_MASK,
							     (SCRUBBER_WORD_VALUE >> 8) & 0xFFu,
							     TENSIX_CFG_ECC_SCRUBBER_WORD);

	tensix_inject_instruction(rmw_byte0, 0, broadcast, noc_x, noc_y);
	tensix_inject_instruction(rmw_byte1, 0, broadcast, noc_x, noc_y);
}

void TensixEccArmIrq(bool broadcast, uint8_t noc_x, uint8_t noc_y)
{
	if (!bh_chip_info_feature_ecc_en()) {
		return;
	}

	/*
	 * Holding the TLB lock keeps a concurrent TensixEccServiceTile / ReadTile from having
	 * its view select replaced between its ECC_CTRL write and ECC_STATUS read. The
	 * broadcast itself goes through the shared TLB 0 like the other tile-init writes.
	 */
	NocEccTlbLock();

	if (broadcast) {
		NOC2AXITensixBroadcastTlbSetup(NOC_ECC_RING, 0, TENSIX_ECC_CTRL,
					       kNoc2AxiOrderingStrict);
		NOC2AXIWrite32(NOC_ECC_RING, 0, TENSIX_ECC_CTRL, ECC_CTRL_ARMED_VALUE);
	} else {
		NOC2AXITlbSetup(NOC_ECC_RING, NOC_ECC_TLB, noc_x, noc_y, TENSIX_ECC_CTRL);
		NOC2AXIWrite32(NOC_ECC_RING, NOC_ECC_TLB, TENSIX_ECC_CTRL, ECC_CTRL_ARMED_VALUE);
	}

	NocEccTlbUnlock();
}

/*
 * Bound on NIU re-read rounds in TensixEccServiceTile. A NIU erroring faster than a few
 * NOC transactions keeps counting after this returns and re-fires on its next edge.
 */
#define SERVICE_MAX_ROUNDS 3

/* Add the NIU counters at the window into @p noc, then zero them. TLB already points here. */
static bool DrainNiuCountersLocked(uint32_t noc[NOC_ECC_NUM_SOURCES], bool first)
{
	uint32_t now[NOC_ECC_NUM_SOURCES];
	bool any = false;

	NocNiuEccReadCountersLocked(NOC_NIU_REGS_BASE, now);
	for (int i = 0; i < NOC_ECC_NUM_SOURCES; i++) {
		if (first) {
			noc[i] = now[i];
		} else {
			noc[i] += now[i];
		}
		any = any || now[i] != 0;
	}

	if (any || first) {
		NocNiuEccClearLocked(NOC_NIU_REGS_BASE, NOC_ECC_SOURCE_MASK);
	}

	return any;
}

/* Pulse ECC_CTRL[4]|[5] then rest armed. TLB already points at this tile. */
static void ClearL1AndIrqLocked(void)
{
	NOC2AXIWrite32(NOC_ECC_RING, NOC_ECC_TLB, TENSIX_ECC_CTRL,
		       ECC_CTRL_ARMED_VALUE | ECC_CTRL_STATUS_CLEAR | ECC_CTRL_IRQ_CLEAR);
	NOC2AXIWrite32(NOC_ECC_RING, NOC_ECC_TLB, TENSIX_ECC_CTRL, ECC_CTRL_ARMED_VALUE);
}

bool TensixEccServiceTile(uint8_t noc_x, uint8_t noc_y, struct tensix_ecc_tile *out)
{
	out->l1_valid = false;
	out->l1_sbe = 0;
	out->l1_dbe = 0;

	NocEccTlbLock();
	NOC2AXITlbSetup(NOC_ECC_RING, NOC_ECC_TLB, noc_x, noc_y, TENSIX_ECC_CTRL);

	/* NIU registers stay readable with the tile clock gated; ECC_STATUS does not. */
	out->clock_gated = NocNiuTileClockGatedLocked(NOC_NIU_REGS_BASE);

	/* Read then clear: the zero drops the NIU level so the next error is a new edge. */
	(void)DrainNiuCountersLocked(out->noc, true);

	if (!out->clock_gated) {
		out->l1_valid = ReadL1CountersLocked(&out->l1_sbe, &out->l1_dbe);

		/*
		 * Zero the L1 counters and drop the interrupt in one pulse, then rest armed.
		 * Clear has priority over set in the manager, so a NOC error whose NIU level
		 * rose between the NIU clear above and the end of this pulse made no edge: it
		 * is in the NIU counter but nothing will ever signal it. Re-read the NIU after
		 * the pulse; draining it again drops the level so the next error is a fresh
		 * edge. No second pulse: [4] would discard L1 counts, and an edge that lands
		 * after the pulse sets the interrupt on its own.
		 */
		ClearL1AndIrqLocked();
		for (int round = 1; round < SERVICE_MAX_ROUNDS; round++) {
			if (!DrainNiuCountersLocked(out->noc, false)) {
				break;
			}
		}
	}

	NocEccTlbUnlock();

	return !out->clock_gated;
}

BUILD_ASSERT((TENSIX_ECC_CTRL >> NOC_TLB_LOG_SIZE) == (NOC_NIU1_REGS_BASE >> NOC_TLB_LOG_SIZE),
	     "Tensix ECC_CTRL and the NOC 1 NIU block must share one NOC2AXI TLB window");

void TensixEccProbe(uint8_t noc_x, uint8_t noc_y, struct tensix_ecc_probe *out)
{
	out->ecc_ctrl = 0;
	out->noc_level = 0;

	NocEccTlbLock();
	NOC2AXITlbSetup(NOC_ECC_RING, NOC_ECC_TLB, noc_x, noc_y, TENSIX_ECC_CTRL);

	out->niu_cfg_0[0] = NocNiuCfg0ReadLocked(NOC_NIU_REGS_BASE);
	out->niu_cfg_0[1] = NocNiuCfg0ReadLocked(NOC_NIU1_REGS_BASE);
	out->clock_gated = (out->niu_cfg_0[0] & NOC_NIU_CFG_0_TILE_CLK_OFF) != 0;
	NocNiuEccReadCountersLocked(NOC_NIU_REGS_BASE, out->noc);

	if (!out->clock_gated) {
		out->ecc_ctrl = NOC2AXIRead32(NOC_ECC_RING, NOC_ECC_TLB, TENSIX_ECC_CTRL);
		out->noc_level =
			TensixEccReadStatusLocked(TENSIX_ECC_SEL_NOC) & TENSIX_ECC_NOC_LEVEL_MASK;
		/* Put the select back; the enables and clear bits were preserved throughout. */
		NOC2AXIWrite32(NOC_ECC_RING, NOC_ECC_TLB, TENSIX_ECC_CTRL, out->ecc_ctrl);
	}

	NocEccTlbUnlock();
}
