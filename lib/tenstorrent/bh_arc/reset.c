/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "chip_info.h"
#include "cm2dm_msg.h"
#include "eth.h"
#include "gddr.h"
#include "harvesting.h"
#include "init.h"
#include "irqnum.h"
#include "noc.h"
#include "noc_init.h"
#include "noc2axi.h"
#include "reg.h"
#include "status_reg.h"
#include "tensix.h"
#include "tensix_init.h"
#include "aiclk_ppm.h"
#include "bh_reset.h"

#include <tenstorrent/bh_power.h>

#include <stdint.h>

#include <tenstorrent/msgqueue.h>
#include <tenstorrent/smc_msg.h>
#include <tenstorrent/post_code.h>
#include <tenstorrent/sys_init_defines.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control/clock_control_tt_bh.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/reset/reset_tt_bh.h>
#include <zephyr/dt-bindings/reset/tt-bh-reset.h>

#define DT_DRV_COMPAT         tenstorrent_bh_clock_control
#define PLL_DEVICE_INIT(inst) DEVICE_DT_INST_GET(inst),

#define TENSIX_NUM_RESET_BANKS TT_BH_RESET_NUM_BANKS(DT_NODELABEL(tensix_reset))

BUILD_ASSERT(TT_BH_RESET_NUM_BANKS(DT_NODELABEL(tensix_reset)) ==
		     TT_BH_RESET_NUM_BANKS(DT_NODELABEL(tensix_risc_reset)),
	     "tensix tile and RISC reset bank counts must match");

static const struct device *const pll_devs[] = {DT_INST_FOREACH_STATUS_OKAY(PLL_DEVICE_INIT)};

static const struct device *const global_reset_dev = DEVICE_DT_GET(DT_NODELABEL(global_reset));
static const struct device *const eth_reset_dev = DEVICE_DT_GET(DT_NODELABEL(eth_reset));
static const struct device *const ddr_reset_dev = DEVICE_DT_GET(DT_NODELABEL(ddr_reset));
static const struct device *const l2cpu_reset_dev = DEVICE_DT_GET(DT_NODELABEL(l2cpu_reset));
static const struct device *const tensix_reset_dev = DEVICE_DT_GET(DT_NODELABEL(tensix_reset));
static const struct device *const tensix_risc_reset_dev =
	DEVICE_DT_GET(DT_NODELABEL(tensix_risc_reset));

/* global_reset reset-mask 0x2383 (includes DEFAULT refclk_cnt_en) */
#define TT_BH_GLOBAL_RESET_MASK                                                                    \
	(BIT(TT_BH_GLOBAL_SYSTEM_RESET_ID) | BIT(TT_BH_GLOBAL_NOC_RESET_ID) |                      \
	 BIT(TT_BH_GLOBAL_REFCLK_CNT_EN_ID) | BIT(TT_BH_GLOBAL_PCIE0_RESET_ID) |                   \
	 BIT(TT_BH_GLOBAL_PCIE1_RESET_ID) | BIT(TT_BH_GLOBAL_PTP_RESET_ID))

#define TT_BH_ETH_TILE_MASK   GENMASK(13, 0)
#define TT_BH_ETH_RISC_MASK   GENMASK(29, 16)
#define TT_BH_DDR_TILE_MASK   GENMASK(7, 0)
#define TT_BH_DDR_RISC_MASK   GENMASK(31, 8)
#define TT_BH_L2CPU_TILE_MASK GENMASK(3, 0)

LOG_MODULE_REGISTER(InitHW, CONFIG_TT_APP_LOG_LEVEL);

uint32_t error_status0;

void record_init_failure(enum init_stage_id stage)
{
	if ((unsigned int)stage >= INIT_STAGE_COUNT) {
		return;
	}

	error_status0 |= BIT(stage);

	WriteReg(STATUS_ERROR_STATUS0_REG_ADDR, error_status0);
}

/* Cable fault mode: true when DMC reports 0W power limit (no cable or improper installation).
 * In this mode, all tiles except column 15 are clock-gated via NIU_CFG_0 TILE_CLK_OFF to minimize
 * power draw, while maintaining the full NOC mesh and ARC-PCIe path for host communication.
 */
static bool cable_fault_mode;

static const uint8_t kNocRing;
static const uint8_t kNocTlb;
static const uint32_t kSoftReset0Addr = 0xFFB121B0; /* NOC address in each tile */
static const uint32_t kAllRiscSoftReset = 0x47800;

bool is_cable_fault_mode(void)
{
	return cable_fault_mode;
}

void bh_soft_reset_all_tensix(void)
{
	/* Broadcast to SOFT_RESET_0 of all Tensixes */
	/* Harvesting is handled by broadcast disables of NocInit */
	NOC2AXITensixBroadcastTlbSetup(kNocRing, kNocTlb, kSoftReset0Addr, kNoc2AxiOrderingStrict);
	NOC2AXIWrite32(kNocRing, kNocTlb, kSoftReset0Addr, kAllRiscSoftReset);
}

/* Assert soft reset for all RISC-V cores */
/* L2CPU is skipped due to JIRA issues BH-25 and BH-28 */
static int AssertSoftResets(void)
{
	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEP6);
	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY) || !IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	/* In cable fault mode, tiles are clock-gated - skip soft reset writes */
	if (cable_fault_mode) {
		return 0;
	}

	/* Assert Soft Reset for ERISC, MRISC Tensix (skip L2CPU due to bug) */
	bh_soft_reset_all_tensix();

	/* Write to SOFT_RESET_0 of ETH */
	for (uint8_t eth_inst = 0; eth_inst < 14; eth_inst++) {
		uint8_t x, y;

		/* Skip harvested ETH tiles */
		if (tile_enable.eth_enabled & BIT(eth_inst)) {
			GetEthNocCoords(eth_inst, kNocRing, &x, &y);
			NOC2AXITlbSetup(kNocRing, kNocTlb, x, y, kSoftReset0Addr);
			NOC2AXIWrite32(kNocRing, kNocTlb, kSoftReset0Addr, kAllRiscSoftReset);
		}
	}

	/* Write to SOFT_RESET_0 of GDDR */
	/* Note that there are 3 NOC nodes for each GDDR instance */
	for (uint8_t gddr_inst = 0; gddr_inst < NUM_GDDR; gddr_inst++) {
		/* Skip harvested GDDR tiles */
		if (tile_enable.gddr_enabled & BIT(gddr_inst)) {
			for (uint8_t noc_node_inst = 0; noc_node_inst < NUM_MRISC_NOC2AXI_PORT;
			     noc_node_inst++) {
				uint8_t x, y;

				GetGddrNocCoords(gddr_inst, noc_node_inst, kNocRing, &x, &y);
				NOC2AXITlbSetup(kNocRing, kNocTlb, x, y, kSoftReset0Addr);
				NOC2AXIWrite32(kNocRing, kNocTlb, kSoftReset0Addr,
					       kAllRiscSoftReset);
			}
		}
	}

	return 0;
}
SYS_INIT_APP(AssertSoftResets);

/* Deassert RISC reset from reset_unit for all RISC-V cores */
/* L2CPU is skipped due to JIRA issues BH-25 and BH-28 */
static int DeassertRiscvResets(void)
{
	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEP7);

	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY) || !IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	/* In cable fault mode, skip RISC-V deasserts to keep cores idle */
	if (cable_fault_mode) {
		return 0;
	}

	/* Go back to PLL bypass, since RISCV resets need to be deasserted at low speed */
	ARRAY_FOR_EACH(pll_devs, i) {
		clock_control_configure(pll_devs[i], NULL,
					(void *)CLOCK_CONTROL_TT_BH_CONFIG_BYPASS);
	}
	/* Deassert RISC reset from reset_unit */
	for (uint32_t i = 0; i < TENSIX_NUM_RESET_BANKS; i++) {
		(void)reset_tt_bh_lines_deassert(tensix_risc_reset_dev, i * TT_BH_RESET_BANK_STRIDE,
						 UINT32_MAX);
	}

	/* Dual-field banks: RMW so tile bits from DeassertTileResets stay set */
	(void)reset_tt_bh_lines_deassert(eth_reset_dev, 0, TT_BH_ETH_RISC_MASK);
	(void)reset_tt_bh_lines_deassert(ddr_reset_dev, 0, TT_BH_DDR_RISC_MASK);

	ARRAY_FOR_EACH(pll_devs, i) {
		clock_control_set_rate(pll_devs[i],
				       (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_INIT_STATE,
				       (clock_control_subsys_rate_t)-1);
	};

	return 0;
}
SYS_INIT_APP(DeassertRiscvResets);

/**
 * @brief Handler for @ref TT_SMC_MSG_TOGGLE_TENSIX_RESET
 * @see toggle_tensix_reset_rqst
 */
static __maybe_unused uint8_t ToggleTensixReset(const union request *req, struct response *rsp)
{
	ARG_UNUSED(req);
	ARG_UNUSED(rsp);

	for (uint32_t i = 0; i < TENSIX_NUM_RESET_BANKS; i++) {
		(void)reset_tt_bh_lines_assert(tensix_reset_dev, i * TT_BH_RESET_BANK_STRIDE,
					       UINT32_MAX);
	}

	for (uint32_t i = 0; i < TENSIX_NUM_RESET_BANKS; i++) {
		(void)reset_tt_bh_lines_deassert(tensix_reset_dev, i * TT_BH_RESET_BANK_STRIDE,
						 UINT32_MAX);
	}

	return 0;
}

#ifndef CONFIG_TT_SMC_RECOVERY
REGISTER_MESSAGE(TT_SMC_MSG_TOGGLE_TENSIX_RESET, ToggleTensixReset);
#endif

static __maybe_unused uint8_t ToggleSingleTensixReset(const union request *req,
						      struct response *rsp)
{
	uint8_t noc_x = req->toggle_single_tensix_reset.noc_x;
	uint8_t noc_y = req->toggle_single_tensix_reset.noc_y;

	/* The host sends logical (post-translation) NOC coordinates.
	 * Convert to physical (pre-translation) so NocToTensixPhysX/Y
	 * can map them to the correct grid indices for the reset registers.
	 */
	uint8_t phys_x, phys_y;

	NocLogicalToPhysical(noc_x, noc_y, &phys_x, &phys_y);

	uint8_t tensix_col = NocToTensixPhysX(phys_x, 0);
	uint8_t tensix_row = NocToTensixPhysY(phys_y, 0);

	if (tensix_col >= NUM_TENSIX_X || tensix_row >= NUM_TENSIX_Y) {
		rsp->data[0] = 1;
		return 1;
	}

	uint8_t tensix_index = (NUM_TENSIX_Y * tensix_col) + tensix_row;
	uint32_t bank_offset = (tensix_index / 32) * TT_BH_RESET_BANK_STRIDE;
	uint8_t bit_index = tensix_index % 32;

	SetAiclkResetSafe(true);

	/* RISC reset assert */
	(void)reset_tt_bh_lines_assert(tensix_risc_reset_dev, bank_offset, BIT(bit_index));

	/* Tile reset assert then deassert */
	(void)reset_tt_bh_lines_assert(tensix_reset_dev, bank_offset, BIT(bit_index));
	(void)reset_tt_bh_lines_deassert(tensix_reset_dev, bank_offset, BIT(bit_index));

	/* The init functions use NOC2AXITlbSetup with
	 * physical NOC coordinates; disable ARC translation first to
	 * prevent double-translation.
	 */
	DisableArcNocTranslation();

	NocInitSingleTile(phys_x, phys_y);
	ProgramNocTranslationSingleTile(phys_x, phys_y);
	EnableTensixCG(false, phys_x, phys_y);

	/* RISC soft reset assert */
	NOC2AXITlbSetup(kNocRing, kNocTlb, phys_x, phys_y, kSoftReset0Addr);
	NOC2AXIWrite32(kNocRing, kNocTlb, kSoftReset0Addr, kAllRiscSoftReset);

	RestoreArcNocTranslation();

	/* RISC reset deassert */
	(void)reset_tt_bh_lines_deassert(tensix_risc_reset_dev, bank_offset, BIT(bit_index));

	SetAiclkResetSafe(false);

	tensix_inject_instruction(TENSIX_INSTRUCTION_UNPACR, 0, false, noc_x, noc_y);

	/* NocInitSingleTile un-gates the tile clock.
	 * If tensix was in low power, clock gate this tile.
	 */
	bool power_state;

	bh_power_state_get(BH_POWER_DOMAIN_TENSIX, &power_state);
	if (!power_state) {
		/* ARC NOC translation has been restored above, so (like the
		 * tensix_inject_instruction call) use the logical coordinates.
		 */
		SetSingleTileClockGate(noc_x, noc_y, true);
	}

	rsp->data[0] = 0;
	return 0;
}

#ifndef CONFIG_TT_SMC_RECOVERY
REGISTER_MESSAGE(TT_SMC_MSG_TOGGLE_SINGLE_TENSIX_RESET, ToggleSingleTensixReset);
#endif

/**
 * @brief Handler for @ref TT_SMC_MSG_REINIT_TENSIX
 * @see reinit_tensix_rqst
 *
 * Redo Tensix init that gets cleared on Tensix reset.
 * This includes all NOC programming and any programming within the tile.
 */
static __maybe_unused uint8_t ReinitTensix(const union request *req, struct response *rsp)
{
	ClearNocTranslation();
	/* We technically don't have to re-program the entire NOC (only the Tensix NOC portions),
	 * but it's simpler to reuse the same functions to re-program all of it.
	 */
	NocInit();
	TensixInit();
	if (bh_chip_info_feature_noc_translation_en()) {
		InitNocTranslationFromHarvesting();
	}

	return 0;
}
#ifndef CONFIG_TT_SMC_RECOVERY
REGISTER_MESSAGE(TT_SMC_MSG_REINIT_TENSIX, ReinitTensix);
#endif

static int DeassertTileResets(void)
{
	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEP3);

	if (!IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	/* Read cable power limit with magic marker check for backward compatibility.
	 * - If magic marker present: new DMC, check power limit (0 = cable fault)
	 * - If magic marker absent: legacy DMC, skip cable fault detection
	 */
	uint32_t raw_value = ReadReg(DMC_CABLE_POWER_LIMIT_REG_ADDR);

	if (bh_chip_info_is_ubb()) {
		/* Galaxy boards have no DMC; CPLD never sets cable power limit */
		LOG_INF("Galaxy board detected, no cable fault check needed");
	} else if ((raw_value & CABLE_POWER_LIMIT_MAGIC_MASK) == CABLE_POWER_LIMIT_MAGIC) {
		/* New DMC with cable power limit feature */
		uint16_t cable_power_limit = raw_value & CABLE_POWER_LIMIT_VALUE_MASK;

		LOG_INF("Cable Power Limit: %u", cable_power_limit);

		if (cable_power_limit == 0) {
			cable_fault_mode = true;
			record_init_failure(INIT_STAGE_CABLE_FAULT);
			LOG_WRN("Cable fault detected (0W power limit). "
				"Entering low-power mode - clock-gating all tiles except column 15 "
				"(contains ARC).");
		}
	} else {
		/* Legacy DMC without cable power limit feature - skip cable fault check */
		LOG_INF("Legacy DMC detected (no cable power feature), skipping cable fault check");
	}

	/* Put all PLLs back into bypass, since tile resets need to be deasserted at low speed */
	ARRAY_FOR_EACH(pll_devs, i) {
		clock_control_configure(pll_devs[i], NULL,
					(void *)CLOCK_CONTROL_TT_BH_CONFIG_BYPASS);
	}

	/* Always deassert NOC, system, and PCIe resets - needed for ARC-PCIe communication.
	 * Absolute writes for dual-field / sparse banks force a known full-word state
	 * (tile half released, RISC half still held). Full tensix banks use line RMW.
	 */
	(void)reset_tt_bh_write(global_reset_dev, TT_BH_GLOBAL_RESET_MASK);
	(void)reset_tt_bh_write(eth_reset_dev, TT_BH_ETH_TILE_MASK);

	for (uint32_t i = 0; i < TENSIX_NUM_RESET_BANKS; i++) {
		(void)reset_tt_bh_lines_deassert(tensix_reset_dev, i * TT_BH_RESET_BANK_STRIDE,
						 UINT32_MAX);
	}

	(void)reset_tt_bh_write(ddr_reset_dev, TT_BH_DDR_TILE_MASK);
	(void)reset_tt_bh_write(l2cpu_reset_dev, TT_BH_L2CPU_TILE_MASK);

	return 0;
}
SYS_INIT_APP(DeassertTileResets);
