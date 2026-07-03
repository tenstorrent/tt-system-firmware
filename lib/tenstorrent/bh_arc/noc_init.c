/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bh_reset.h"
#include "chip_info.h"
#include "harvesting.h"
#include "noc_init.h"
#include "noc.h"
#include "noc2axi.h"
#include "reg.h"
#include "telemetry.h"
#include "gddr.h"
#include "tensix_state_msg.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <tenstorrent/msgqueue.h>
#include <tenstorrent/smc_msg.h>
#include <tenstorrent/sys_init_defines.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/zbus/zbus.h>

/* NIU config register indices for Read/WriteNocCfgReg */
#define NIU_CFG_0                    0x0
#define ROUTER_CFG(n)                ((n) + 1)
#define NOC_X_ID_TRANSLATE_TABLE(n)  ((n) + 0x6)
#define NOC_Y_ID_TRANSLATE_TABLE(n)  ((n) + 0xC)
#define NOC_ID_LOGICAL               0x12
#define NOC_ID_TRANSLATE_COL_MASK    0x14
#define NOC_ID_TRANSLATE_ROW_MASK    0x15
#define DDR_COORD_TRANSLATE_TABLE(n) ((n) + 0x16)

/* NIU_CFG_0 fields */
#define NIU_CFG_0_TILE_CLK_OFF          12
#define NIU_CFG_0_TILE_HEADER_STORE_OFF 13 /* NOC2AXI only */
#define NIU_CFG_0_NOC_ID_TRANSLATE_EN   14
#define NIU_CFG_0_AXI_SLAVE_ENABLE      15

#define NOC_TRANSLATE_ID_WIDTH      5
#define NOC_TRANSLATE_TABLE_XY_SIZE (32 / NOC_TRANSLATE_ID_WIDTH)

/* Overlay stream registers & fields */
#define STREAM_PERF_CONFIG_REG_INDEX 35
#define CLOCK_GATING_EN              0

static const uint32_t kNoc0RegBase = 0x80050000;
static const uint32_t kNoc1RegBase = 0x80058000;
static const uint32_t kNiuCfg0Offset = 0x100 + 4 * NIU_CFG_0;

static const uint8_t kTlbIndex;

static const uint32_t kFirstCfgRegIndex = 0x100 / sizeof(uint32_t);

static bool noc_translation_enabled;

static volatile void *SetupNiuTlbPhys(uint8_t tlb_index, uint8_t px, uint8_t py, uint8_t noc_id)
{
	uint64_t regs = NiuRegsBase(px, py, noc_id);

	NOC2AXITlbSetup(noc_id, tlb_index, PhysXToNoc(px, noc_id), PhysYToNoc(py, noc_id), regs);

	return GetTlbWindowAddr(noc_id, tlb_index, regs);
}

static volatile void *SetupNiuTlb(uint8_t tlb_index, uint8_t nx, uint8_t ny, uint8_t noc_id)
{
	uint64_t regs = NiuRegsBase(NocToPhysX(nx, noc_id), NocToPhysY(ny, noc_id), noc_id);

	NOC2AXITlbSetup(noc_id, tlb_index, nx, ny, regs);

	return GetTlbWindowAddr(noc_id, tlb_index, regs);
}

static uint32_t ReadNocCfgReg(volatile void *regs, uint32_t cfg_reg_index)
{
	uint32_t address = (uint32_t)regs + sizeof(uint32_t) * (kFirstCfgRegIndex + cfg_reg_index);

	return ReadReg(address);
}

static void WriteNocCfgReg(volatile void *regs, uint32_t cfg_reg_index, uint32_t value)
{
	uint32_t address = (uint32_t)regs + sizeof(uint32_t) * (kFirstCfgRegIndex + cfg_reg_index);

	WriteReg(address, value);
}

static void EnableOverlayCg(uint8_t tlb_index, uint8_t px, uint8_t py)
{
	uint8_t ring = 0; /* Either NOC ring works, there's only one overlay. */

	uint64_t overlay_regs_base = OverlayRegsBase(px, py);

	if (overlay_regs_base != 0) {
		NOC2AXITlbSetup(ring, tlb_index, PhysXToNoc(px, ring), PhysYToNoc(py, ring),
				overlay_regs_base);

		volatile uint32_t *regs = GetTlbWindowAddr(ring, tlb_index, overlay_regs_base);

		/* Set stream[0].STREAM_PERF_CONFIG.CLOCK_GATING_EN = 1, leave other fields at
		 * defaults.
		 */
		regs[STREAM_PERF_CONFIG_REG_INDEX] |= 1u << CLOCK_GATING_EN;
	}
}

/* This function requires that NOC translation is disabled (or identity) on both NOCs for the ARC
 * node.
 */
static void ProgramBroadcastExclusion(uint16_t disabled_tensix_columns)
{
	/* ROUTER_CFG_1,2 are a 64-bit mask for column broadcast disable */
	/* ROUTER_CFG_3,4 are a 64-bit mask for row broadcast disable */
	/* A node will not receive broadcasts if it is in a disabled row or column. */

	/* Disable broadcast to west GDDR, L2CPU/security/ARC, east GDDR columns. */
	uint32_t router_cfg_1[NUM_NOCS] = {
		BIT(0) | BIT(8) | BIT(9),
		BIT(NOC0_X_TO_NOC1(0)) | BIT(NOC0_X_TO_NOC1(8)) | BIT(NOC0_X_TO_NOC1(9)),
	};

	/* Disable broadcast to ethernet row, PCIE/SERDES row. */
	static const uint32_t router_cfg_3[NUM_NOCS] = {
		BIT(0) | BIT(1),
		BIT(NOC0_Y_TO_NOC1(0)) | BIT(NOC0_Y_TO_NOC1(1)),
	};

	/* Update for any disabled Tensix columns. */
	for (uint8_t i = 0; i < 14; i++) {
		if (disabled_tensix_columns & BIT(i)) {
			uint8_t noc0_x = TensixPhysXToNoc(i, 0);

			router_cfg_1[0] |= BIT(noc0_x);
			router_cfg_1[1] |= BIT(NOC0_X_TO_NOC1(noc0_x));
		}
	}

	for (uint32_t py = 0; py < NOC_Y_SIZE; py++) {
		for (uint32_t px = 0; px < NOC_X_SIZE; px++) {
			for (uint32_t noc_id = 0; noc_id < NUM_NOCS; noc_id++) {
				volatile uint32_t *noc_regs =
					SetupNiuTlbPhys(kTlbIndex, px, py, noc_id);

				WriteNocCfgReg(noc_regs, ROUTER_CFG(1), router_cfg_1[noc_id]);
				WriteNocCfgReg(noc_regs, ROUTER_CFG(2), 0);
				WriteNocCfgReg(noc_regs, ROUTER_CFG(3), router_cfg_3[noc_id]);
				WriteNocCfgReg(noc_regs, ROUTER_CFG(4), 0);
			}
		}
	}
}

static bool GetTileClkDisable(uint8_t px, uint8_t py)
{
	/* Tile clock disable for disabled Tensix columns */
	if (px >= 1 && px <= 14 && py >= 2) {
		uint8_t tensix_x = px - 1;

		return !IS_BIT_SET(tile_enable.tensix_col_enabled, tensix_x);
	} else if (px >= 1 && px <= 14 && py == 1) {
		/* ETH tiles */
		uint8_t eth_inst = px - 1;

		return !IS_BIT_SET(tile_enable.eth_enabled, eth_inst);
	} else if (px == 0) {
		/* Leftmost column, GDDR 0-3 */
		uint8_t gddr_inst = py / 3;

		return !IS_BIT_SET(tile_enable.gddr_enabled, gddr_inst);
	} else if (px == 16) {
		/* Rightmost column, GDDR 4-7 */
		uint8_t gddr_inst = 4 + py / 3;

		return !IS_BIT_SET(tile_enable.gddr_enabled, gddr_inst);
	}
	return false;
}

/* clang-format off */
ZBUS_CHAN_DEFINE(tensix_state_chan,
	struct tensix_state_msg,
	NULL, /* validator */
	NULL, /* user data */
	ZBUS_OBSERVERS_EMPTY,
	ZBUS_MSG_INIT(.enable = true)
);
/* clang-format on */

int32_t set_tensix_enable(bool enable)
{
	const uint8_t noc_ring = 0;
	const uint8_t noc_tlb = 0;
	uint8_t x;
	uint8_t y;

	GetEnabledTensix(&x, &y);

	volatile uint32_t *noc_regs = SetupNiuTlb(kTlbIndex, x, y, 0);

	uint32_t niu_cfg_0 = ReadNocCfgReg(noc_regs, NIU_CFG_0);

	WRITE_BIT(niu_cfg_0, NIU_CFG_0_TILE_CLK_OFF, !enable);
	uint32_t niu_cfg_0_addr = 0xFFB20100;

	NOC2AXITensixBroadcastTlbSetup(noc_ring, noc_tlb, niu_cfg_0_addr, kNoc2AxiOrderingStrict);
	NOC2AXIWrite32(noc_ring, noc_tlb, niu_cfg_0_addr, niu_cfg_0);

	noc_regs = SetupNiuTlb(kTlbIndex, x, y, 0);

	struct tensix_state_msg tensix_state = {enable};

	zbus_chan_pub(&tensix_state_chan, &tensix_state, K_NO_WAIT);

	return 0;
}

void SetSingleTileClockGate(uint8_t noc0_x, uint8_t noc0_y, bool gate)
{
	volatile uint32_t *noc_regs = SetupNiuTlb(kTlbIndex, noc0_x, noc0_y, 0);

	uint32_t niu_cfg_0 = ReadNocCfgReg(noc_regs, NIU_CFG_0);

	WRITE_BIT(niu_cfg_0, NIU_CFG_0_TILE_CLK_OFF, gate);
	WriteNocCfgReg(noc_regs, NIU_CFG_0, niu_cfg_0);
}

int NocInit(void)
{
	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY) || !IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	for (uint32_t py = 0; py < NOC_Y_SIZE; py++) {
		for (uint32_t px = 0; px < NOC_X_SIZE; px++) {
			NocInitSingleTile(PhysXToNoc(px, 0), PhysYToNoc(py, 0));
		}
	}

	/* Skip broadcast exclusion in cable fault mode — tensix tiles are clock-gated. */
	if (!is_cable_fault_mode()) {
		uint16_t bad_tensix_cols = BIT_MASK(14) & ~tile_enable.tensix_col_enabled;

		ProgramBroadcastExclusion(bad_tensix_cols);
	}

	return 0;
}
SYS_INIT_APP(NocInit);

void NocInitSingleTile(uint8_t noc0_x, uint8_t noc0_y)
{
	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY) || !IS_ENABLED(CONFIG_ARC)) {
		return;
	}

	uint8_t px = NocToPhysX(noc0_x, 0);
	uint8_t py = NocToPhysY(noc0_y, 0);

	uint32_t niu_cfg_0_updates = BIT(NIU_CFG_0_TILE_HEADER_STORE_OFF);
	uint32_t router_cfg_0_updates = 0xF << 8;

	bool cg_en = bh_chip_info_feature_cg_en();

	if (cg_en) {
		niu_cfg_0_updates |= BIT(0);
		router_cfg_0_updates |= BIT(0);
	}

	bool tile_clk_off;

	if (is_cable_fault_mode()) {
		tile_clk_off = (px != 15);
	} else {
		tile_clk_off = GetTileClkDisable(px, py);
	}

	for (uint32_t noc_id = 0; noc_id < NUM_NOCS; noc_id++) {
		volatile uint32_t *noc_regs = SetupNiuTlbPhys(kTlbIndex, px, py, noc_id);

		uint32_t niu_cfg_0 = ReadNocCfgReg(noc_regs, NIU_CFG_0);

		niu_cfg_0 |= niu_cfg_0_updates;
		WRITE_BIT(niu_cfg_0, NIU_CFG_0_TILE_CLK_OFF, tile_clk_off);
		WriteNocCfgReg(noc_regs, NIU_CFG_0, niu_cfg_0);

		uint32_t router_cfg_0 = ReadNocCfgReg(noc_regs, ROUTER_CFG(0));

		router_cfg_0 |= router_cfg_0_updates;
		WriteNocCfgReg(noc_regs, ROUTER_CFG(0), router_cfg_0);
	}

	if (cg_en) {
		EnableOverlayCg(kTlbIndex, px, py);
	}
}

#define PRE_TRANSLATION_SIZE 32

struct NocTranslation {
	bool translate_en;
	uint8_t translate_table_x[PRE_TRANSLATION_SIZE];
	uint8_t translate_table_y[PRE_TRANSLATION_SIZE];

	uint32_t translate_col_mask[DIV_ROUND_UP(PRE_TRANSLATION_SIZE, 32)];
	uint32_t translate_row_mask[DIV_ROUND_UP(PRE_TRANSLATION_SIZE, 32)];

	uint16_t logical_coords[NOC_X_SIZE][NOC_Y_SIZE];
};

static struct NocTranslation translation[NUM_NOCS];

static void SetLogicalCoord(struct NocTranslation *nt, uint8_t post_x, uint8_t post_y,
			    uint8_t logical_x, uint8_t logical_y)
{
	nt->logical_coords[post_x][post_y] = ((uint32_t)logical_y << 6) | logical_x;
}

static void MakeIdentity(struct NocTranslation *nt)
{
	memset(nt, 0, sizeof(*nt));

	nt->translate_en = true;

	for (unsigned int i = 0; i < PRE_TRANSLATION_SIZE; i++) {
		nt->translate_table_x[i] = (i < NOC_X_SIZE) ? i : 0;
		nt->translate_table_y[i] = (i < NOC_Y_SIZE) ? i : 0;
	}

	for (unsigned int x = 0; x < NOC_X_SIZE; x++) {
		for (unsigned int y = 0; y < NOC_Y_SIZE; y++) {
			SetLogicalCoord(nt, x, y, x, y);
		}
	}
}

static void CopyNoc0ToNoc1(const struct NocTranslation *noc0, struct NocTranslation *noc1)
{
	*noc1 = *noc0;

	for (unsigned int i = 0; i < PRE_TRANSLATION_SIZE; i++) {
		noc1->translate_table_x[i] = NOC_X_SIZE - noc0->translate_table_x[i] - 1;
		noc1->translate_table_y[i] = NOC_Y_SIZE - noc0->translate_table_y[i] - 1;
	}

	for (unsigned int x = 0; x < NOC_X_SIZE; x++) {
		for (unsigned int y = 0; y < NOC_Y_SIZE; y++) {
			noc1->logical_coords[x][y] =
				noc0->logical_coords[NOC_X_SIZE - x - 1][NOC_Y_SIZE - y - 1];
		}
	}
}

/* https://tenstorrent.sharepoint.com/:x:/r/sites/SOC/Blackhole%20Documents/
 * Blackhole%20-%20NOC%20Co-ordinates.xlsx?
 * d=w449397eff6fc48abaed13762398c30dd&csf=1&web=1&e=G4HRZp
 */
/* X coordinate of tensix_with_l1[i][j] (for all j) and tt_eth_ss[i] */
static const uint8_t kTensixEthNoc0X[] = {1, 16, 2, 15, 3, 14, 4, 13, 5, 12, 6, 11, 7, 10};
/* Y coordinate of l2cpu_ss_inst[i]-P1. */
static const uint8_t kL2CpuNoc0Y[] = {3, 9, 5, 7};
/* Y coordinates of GDDR[i] and GDDR[i+4]. Order of ports within controllers doesn't matter. */
static const uint8_t kGddrY[][3] = {{0, 1, 11}, {2, 10, 3}, {9, 4, 8}, {5, 7, 6}};

static void CopyBytesSkipIndices(uint8_t *out, const uint8_t *in, size_t count, uint32_t skip_mask)
{
	uint8_t *out_end = out + count;

	while (out != out_end) {
		if (!(skip_mask & 1)) {
			*out++ = *in;
		}

		skip_mask >>= 1;
		in++;
	}
}

/* NOC Overlay needs a "logical coordinate" for each node.
 * We can't derive this from the translation tables alone because there may be many unintentional
 * aliases for each node. But within the given pre-translation coordinate box, there must not be any
 * aliases.
 */
static void ApplyLogicalCoords(struct NocTranslation *nt, uint8_t post_x_start,
			       uint8_t post_y_start, uint8_t post_x_end, uint8_t post_y_end,
			       uint8_t pre_x_start, uint8_t pre_y_start, uint8_t pre_x_end,
			       uint8_t pre_y_end)
{
	for (unsigned int pre_x = pre_x_start; pre_x <= pre_x_end; pre_x++) {

		unsigned int post_x = nt->translate_table_x[pre_x];

		if (post_x < post_x_start || post_x > post_x_end) {
			continue;
		}

		for (unsigned int pre_y = pre_y_start; pre_y <= pre_y_end; pre_y++) {

			unsigned int post_y = nt->translate_table_y[pre_y];

			if (post_y < post_y_start || post_y > post_y_end) {
				continue;
			}

			SetLogicalCoord(nt, post_x, post_y, pre_x, pre_y);
		}
	}
}

/* This function assumes that NOC translation is disabled or identity for the ARC node. */
static void ProgramNocTranslation(void)
{
	const uint8_t arc_noc0_x = 8;
	const uint8_t arc_noc0_y = 0;

	bool enabling = translation[0].translate_en;

	for (uint32_t py = 0; py < NOC_Y_SIZE; py++) {
		for (uint32_t px = 0; px < NOC_X_SIZE; px++) {
			uint8_t noc0_x = PhysXToNoc(px, 0);
			uint8_t noc0_y = PhysYToNoc(py, 0);

			/* ARC must be the very last tile to get translation enabled,
			 * because there's no embedded identity map.
			 */
			if (enabling && noc0_x == arc_noc0_x && noc0_y == arc_noc0_y) {
				continue;
			}

			ProgramNocTranslationSingleTile(noc0_x, noc0_y);
		}
	}

	if (enabling) {
		ProgramNocTranslationSingleTile(arc_noc0_x, arc_noc0_y);
	}
}

/* Please see
 * https://docs.google.com/spreadsheets/d/1tGG4UPfABXrd97Y3VPJS7CusDFlamcml9kEw1HEwrlM/edit?usp=sharing
 * and also the python reference code.
 */
static struct NocTranslation ComputeNocTranslation(unsigned int pcie_instance,
						   uint16_t bad_tensix_cols, uint8_t bad_gddr,
						   uint16_t skip_eth)
{
	struct NocTranslation noc0;

	MakeIdentity(&noc0);

	/* Block column translations on PCIE and ethernet rows. Column translations will only affect
	 * the Tensix rows.
	 */
	noc0.translate_row_mask[0] |= BIT(0) | BIT(1);

	/* Tensix
	 * bad_tensix_cols is a bitmap of i as in tensix_with_l1[i][j].
	 * We want to fill out the tensix NOC columns (1-7, 10-16) in increasing order, except
	 * skipping the disabled columns which will be moved to the end.
	 */

	const unsigned int num_tensix_cols = ARRAY_SIZE(kTensixEthNoc0X);

	/* Convert physical tensix column bits into NOC 0 X bits. */
	unsigned int good_tensix_noc0_x = GENMASK(7, 1) | GENMASK(16, 10);

	for (unsigned int i = 0; i < num_tensix_cols; i++) {
		if (bad_tensix_cols & BIT(i)) {
			good_tensix_noc0_x &= ~BIT(kTensixEthNoc0X[i]);
		}
	}

	for (unsigned int noc_x = 1; noc_x <= 7 && good_tensix_noc0_x; noc_x++) {
		/* pop lowest bit, it's the next valid column */
		unsigned int good_tensix_lsb = LSB_GET(good_tensix_noc0_x);

		good_tensix_noc0_x &= ~good_tensix_lsb;
		unsigned int next_good_tensix = LOG2(good_tensix_lsb);

		noc0.translate_table_x[noc_x] = next_good_tensix;
	}

	for (unsigned int noc_x = 10; noc_x <= 16 && good_tensix_noc0_x; noc_x++) {
		/* pop lowest bit, it's the next valid column */
		unsigned int good_tensix_lsb = LSB_GET(good_tensix_noc0_x);

		good_tensix_noc0_x &= ~good_tensix_lsb;
		unsigned int next_good_tensix = LOG2(good_tensix_lsb);

		noc0.translate_table_x[noc_x] = next_good_tensix;
	}

	/* This assumes that there are no more than 7 bad columns.
	 * It only updates cols 10-16 and never looks back into cols 1-7.
	 */
	for (unsigned int noc_x = 16; noc_x >= 10 && bad_tensix_cols; noc_x--) {
		unsigned int bad_tensix_lsb = LSB_GET(bad_tensix_cols);

		bad_tensix_cols &= ~bad_tensix_lsb;
		unsigned int next_bad_tensix = kTensixEthNoc0X[LOG2(bad_tensix_lsb)];

		noc0.translate_table_x[noc_x] = next_bad_tensix;
	}

	ApplyLogicalCoords(&noc0, 1, 2, 7, 11, 1, 2, 16, 11);
	ApplyLogicalCoords(&noc0, 10, 2, 16, 11, 1, 2, 16, 11);

	/* GDDR */
	if (bad_gddr >= 4) { /* includes all GDDR good */
		/* Put columns in west/east order. If there's a bad GDDR, it's in east. */
		noc0.translate_table_x[17] = 0; /* West */
		noc0.translate_table_x[18] = 9; /* East */
	} else {
		/* Bad GDDR is in west. */
		noc0.translate_table_x[17] = 9; /* East */
		noc0.translate_table_x[18] = 0; /* West */
	}

	uint8_t gddr_y_order[] = {0, 1, 2, 3};

	if (bad_gddr != NO_BAD_GDDR) {
		/* Move bad_gddr to the end (highest Y). */
		uint8_t bad_gddr_row = bad_gddr % 4;

		memmove(gddr_y_order + bad_gddr_row, gddr_y_order + bad_gddr_row + 1,
			ARRAY_SIZE(gddr_y_order) - bad_gddr_row - 1);
		gddr_y_order[3] = bad_gddr % 4;
	}

	for (unsigned int gddr = 0; gddr < ARRAY_SIZE(gddr_y_order); gddr++) {
		memcpy(&noc0.translate_table_y[12 + gddr * 3], kGddrY[gddr_y_order[gddr]], 3);
	}

	ApplyLogicalCoords(&noc0, 0, 0, 0, 11, 17, 12, 18, 23);
	ApplyLogicalCoords(&noc0, 9, 0, 9, 11, 17, 12, 18, 23);

	/* PCIE
	 * 19-24 => 2-0 or 11-0, whichever is in use as the endpoint.
	 */
	unsigned int pcie_x = pcie_instance ? 11 : 2;

	noc0.translate_table_x[19] = pcie_x;
	noc0.translate_table_y[24] = 0;

	ApplyLogicalCoords(&noc0, pcie_x, 0, pcie_x, 0, 19, 24, 19, 24);

	/* Ethernet
	 * 20-25..31-25 => X-1 where X is rearranged to give a predictable mapping from NOC
	 * coordinate to SERDES.
	 */
	noc0.translate_table_y[25] = 1;
	CopyBytesSkipIndices(noc0.translate_table_x + 20, kTensixEthNoc0X, 12, skip_eth);
	ApplyLogicalCoords(&noc0, 1, 1, 7, 1, 20, 25, 31, 25);
	ApplyLogicalCoords(&noc0, 10, 1, 16, 1, 20, 25, 31, 25);

	/* L2CPU
	 * 8-26,27,28,29 => 8-3,9,5,7
	 * This puts l2cpu_ss_inst[i]-P1 in order by i.
	 */
	memcpy(noc0.translate_table_y + 26, kL2CpuNoc0Y, ARRAY_SIZE(kL2CpuNoc0Y));
	/* L2CPU are on row X=8 which is same pre/post translated */
	ApplyLogicalCoords(&noc0, 8, 3, 8, 9, 8, 26, 8, 29);

	/* Security
	 * 8-30 => 8-2
	 */
	noc0.translate_table_y[30] = 2;
	ApplyLogicalCoords(&noc0, 8, 2, 8, 2, 8, 30, 8, 30);

	return noc0;
}

/* This function assumes that NOC translation is disabled (or identity on 17x12) for the ARC node
 * when called.
 */
void InitNocTranslation(unsigned int pcie_instance, uint16_t bad_tensix_cols, uint8_t bad_gddr,
			uint16_t skip_eth)
{
	translation[0] = ComputeNocTranslation(pcie_instance, bad_tensix_cols, bad_gddr, skip_eth);
	CopyNoc0ToNoc1(&translation[0], &translation[1]);

	ProgramNocTranslation();

	UpdateTelemetryNocTranslation(true);

	noc_translation_enabled = true;
}

int InitNocTranslationFromHarvesting(void)
{
	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY) || !IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	/* In cable fault mode, tiles are clock-gated - skip NOC translation
	 * programming since tiles are not active.
	 */
	if (is_cable_fault_mode()) {
		return 0;
	}

	if (!bh_chip_info_feature_noc_translation_en()) {
		return 0;
	}

	/* Set up the EP as pcie instance (at 19-24). If there's no EP, it doesn't matter. */
	unsigned int pcie_instance;

	if (tile_enable.pcie_usage[0] == BH_PCIE_MODE_EP) {
		pcie_instance = 0;
	} else {
		pcie_instance = 1;
	}

	uint16_t bad_tensix_cols = BIT_MASK(14) & ~tile_enable.tensix_col_enabled;

	uint8_t bad_gddr = find_lsb_set(~(uint32_t)tile_enable.gddr_enabled) - 1;

	if (bad_gddr == 8) {
		bad_gddr = NO_BAD_GDDR;
	}

	/* At least one of 4,5,6 is not enabled. Use the last one not enabled
	 * as the one to not skip. Ditto 7,8,9.
	 */
	uint16_t skip_eth;

	skip_eth = 1 << (find_msb_set(~tile_enable.eth_enabled & GENMASK(6, 4)) - 1);
	skip_eth |= 1 << (find_msb_set(~tile_enable.eth_enabled & GENMASK(9, 7)) - 1);

	InitNocTranslation(pcie_instance, bad_tensix_cols, bad_gddr, skip_eth);

	return 0;
}
SYS_INIT_APP(InitNocTranslationFromHarvesting);

void ProgramNocTranslationSingleTile(uint8_t noc0_x, uint8_t noc0_y)
{
	uint8_t px = NocToPhysX(noc0_x, 0);
	uint8_t py = NocToPhysY(noc0_y, 0);

	for (unsigned int noc_id = 0; noc_id < NUM_NOCS; noc_id++) {
		const struct NocTranslation *nt = &translation[noc_id];

		uint32_t packed_x[NOC_TRANSLATE_TABLE_XY_SIZE] = {};
		uint32_t packed_y[NOC_TRANSLATE_TABLE_XY_SIZE] = {};

		for (unsigned int i = 0; i < PRE_TRANSLATION_SIZE; i++) {
			uint32_t index = i / NOC_TRANSLATE_TABLE_XY_SIZE;
			uint32_t shift = i % NOC_TRANSLATE_TABLE_XY_SIZE * NOC_TRANSLATE_ID_WIDTH;

			packed_x[index] |= (uint32_t)nt->translate_table_x[i] << shift;
			packed_y[index] |= (uint32_t)nt->translate_table_y[i] << shift;
		}

		uint8_t ring_x = PhysXToNoc(px, noc_id);
		uint8_t ring_y = PhysYToNoc(py, noc_id);

		volatile void *noc_regs = SetupNiuTlbPhys(kTlbIndex, px, py, noc_id);

		uint32_t niu_cfg_0 = ReadNocCfgReg(noc_regs, NIU_CFG_0);

		if (!nt->translate_en) {
			WRITE_BIT(niu_cfg_0, NIU_CFG_0_NOC_ID_TRANSLATE_EN, 0);
			WriteNocCfgReg(noc_regs, NIU_CFG_0, niu_cfg_0);
		}

		WriteNocCfgReg(noc_regs, NOC_ID_TRANSLATE_COL_MASK, nt->translate_col_mask[0]);
		WriteNocCfgReg(noc_regs, NOC_ID_TRANSLATE_ROW_MASK, nt->translate_row_mask[0]);
		WriteNocCfgReg(noc_regs, DDR_COORD_TRANSLATE_TABLE(5), 0);
		WriteNocCfgReg(noc_regs, NOC_ID_LOGICAL, nt->logical_coords[ring_x][ring_y]);

		for (unsigned int i = 0; i < ARRAY_SIZE(packed_x); i++) {
			WriteNocCfgReg(noc_regs, NOC_X_ID_TRANSLATE_TABLE(i), packed_x[i]);
			WriteNocCfgReg(noc_regs, NOC_Y_ID_TRANSLATE_TABLE(i), packed_y[i]);
		}

		if (nt->translate_en) {
			WRITE_BIT(niu_cfg_0, NIU_CFG_0_NOC_ID_TRANSLATE_EN, 1);
			WriteNocCfgReg(noc_regs, NIU_CFG_0, niu_cfg_0);
		}
	}
}

void DisableArcNocTranslation(void)
{
	/* Program direct rather than relying on NOC loopback, because we
	 * don't know what the pre-translation ARC coordinates are.
	 */
	uint32_t niu_cfg_0 = ReadReg(kNoc0RegBase + kNiuCfg0Offset);

	WRITE_BIT(niu_cfg_0, NIU_CFG_0_NOC_ID_TRANSLATE_EN, 0);
	WriteReg(kNoc0RegBase + kNiuCfg0Offset, niu_cfg_0);

	niu_cfg_0 = ReadReg(kNoc1RegBase + kNiuCfg0Offset);
	WRITE_BIT(niu_cfg_0, NIU_CFG_0_NOC_ID_TRANSLATE_EN, 0);
	WriteReg(kNoc1RegBase + kNiuCfg0Offset, niu_cfg_0);
}

void EnableArcNocTranslation(void)
{
	uint32_t niu_cfg_0 = ReadReg(kNoc0RegBase + kNiuCfg0Offset);

	WRITE_BIT(niu_cfg_0, NIU_CFG_0_NOC_ID_TRANSLATE_EN, 1);
	WriteReg(kNoc0RegBase + kNiuCfg0Offset, niu_cfg_0);

	niu_cfg_0 = ReadReg(kNoc1RegBase + kNiuCfg0Offset);
	WRITE_BIT(niu_cfg_0, NIU_CFG_0_NOC_ID_TRANSLATE_EN, 1);
	WriteReg(kNoc1RegBase + kNiuCfg0Offset, niu_cfg_0);
}

void RestoreArcNocTranslation(void)
{
	if (translation[0].translate_en) {
		EnableArcNocTranslation();
	}
}

void ClearNocTranslation(void)
{
	DisableArcNocTranslation();

	struct NocTranslation all_zeroes = {
		0,
	};

	for (unsigned int x = 0; x < NOC_X_SIZE; x++) {
		for (unsigned int y = 0; y < NOC_Y_SIZE; y++) {
			SetLogicalCoord(&all_zeroes, x, y, x, y);
		}
	}

	translation[0] = all_zeroes;
	translation[1] = all_zeroes;

	ProgramNocTranslation();

	UpdateTelemetryNocTranslation(false);

	noc_translation_enabled = false;
}

/**
 * @brief Handler for @ref TT_SMC_MSG_DEBUG_NOC_TRANSLATION messages
 *
 * @details Re-programs NOC (Network on Chip) coordinate translation for debug and testing purposes.
 *          NOC translation is used to route around defective tiles (e.g. Tensix columns, ETH tiles,
 *          GDDR tiles).
 *
 * @param req Pointer to the host request message. use @ref request::debug_noc_translation for
 *            structured access
 * @param rsp Pointer to the response message to be sent back to host
 *
 * @return 0 on success
 * @return non-zero on error
 * @see debug_noc_translation_rqst
 */
static uint8_t debug_noc_translation_handler(const union request *req, struct response *rsp)
{
	bool enable_translation = req->debug_noc_translation.enable_translation;
	unsigned int pcie_instance = req->debug_noc_translation.pcie_instance;
	bool pcie_instance_override = req->debug_noc_translation.pcie_instance_override;
	uint16_t bad_tensix_cols = req->debug_noc_translation.bad_tensix_cols;

	uint8_t bad_gddr = req->debug_noc_translation.bad_gddr;
	uint16_t skip_eth = req->debug_noc_translation.skip_eth_low |
			    ((uint16_t)req->debug_noc_translation.skip_eth_hi << 8U);

	if (bad_gddr >= NUM_GDDR && bad_gddr != NO_BAD_GDDR) {
		return -EINVAL;
	}
	ClearNocTranslation();

	ProgramBroadcastExclusion(bad_tensix_cols);

	if (enable_translation) {
		if (!pcie_instance_override) {
			struct bh_pci_property pci1;

			bh_chip_info_pci_property(1, &pci1);
			if (pci1.pcie_mode == BH_PCIE_MODE_EP) {
				pcie_instance = 1;
			} else {
				pcie_instance = 0;
			}
		}

		InitNocTranslation(pcie_instance, bad_tensix_cols, bad_gddr, skip_eth);
	}

	return 0;
}

REGISTER_MESSAGE(TT_SMC_MSG_DEBUG_NOC_TRANSLATION, debug_noc_translation_handler);

void GetEnabledTensix(uint8_t *x, uint8_t *y)
{
	if (noc_translation_enabled) {
		/* There's always at least one enabled Tensix column, and when translation is
		 * enabled, it's at X=1.
		 */
		*x = 1;
	} else {
		uint8_t physical_tensix_x = LOG2(LSB_GET(tile_enable.tensix_col_enabled));

		*x = kTensixEthNoc0X[physical_tensix_x];
	}
	*y = 2;
}

bool IsNocTranslationEnabled(void)
{
	return noc_translation_enabled;
}

void NocLogicalToPhysical(uint8_t logical_x, uint8_t logical_y, uint8_t *phys_x, uint8_t *phys_y)
{
	const struct NocTranslation *nt = &translation[0];

	if (nt->translate_en && !IS_BIT_SET(nt->translate_row_mask[0], logical_y)) {
		*phys_x = nt->translate_table_x[logical_x];
	} else {
		*phys_x = logical_x;
	}

	if (nt->translate_en && !IS_BIT_SET(nt->translate_col_mask[0], logical_x)) {
		*phys_y = nt->translate_table_y[logical_y];
	} else {
		*phys_y = logical_y;
	}
}
