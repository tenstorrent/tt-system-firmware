/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Harvesting tile-enable state.
 *
 * The tile_enable defaults below (everything enabled) are the values SMC
 * recovery runs with: recovery references tile_enable (e.g. reset.c skips
 * harvested ETH/GDDR tiles) but never computes harvesting. The fuse- and
 * firmware-table-driven computation needs the bh_fwtable driver, so it is
 * compiled only when CONFIG_BH_FWTABLE is set (mission firmware, not recovery).
 */

#include "harvesting.h"

#include <zephyr/sys/util.h>

#ifdef CONFIG_BH_FWTABLE
#include "functional_efuse.h"
#include "noc.h"

#include <tenstorrent/sys_init_defines.h>
#include <zephyr/drivers/misc/bh_fwtable.h>
#include <zephyr/init.h>
#endif

TileEnable tile_enable = {
	.tensix_col_enabled = BIT_MASK(14),
	.eth_enabled = BIT_MASK(14),
	.eth5_serdes = true,
	.eth8_serdes = true,
	.eth_serdes_connected = BIT_MASK(12),
	.gddr_enabled = BIT_MASK(8),
	.l2cpu_enabled = BIT_MASK(4),
	.pcie_enabled = BIT_MASK(2),
};

#ifdef CONFIG_BH_FWTABLE
static const struct device *const fwtable_dev = DEVICE_DT_GET(DT_NODELABEL(fwtable));

static bool FusesValid(void)
{
	/* ASIC_ID_OLD is the old location of the ASIC ID */
	/* This location was moved when we started fusing harvesting information */
	/* We want to ignore anything before this point because ATE fuses were fused */
	/* incorrectly for many of these parts, corrupting unrelated regions */
	return READ_FUNCTIONAL_EFUSE(ASIC_ID_OLD) == 0;
}

static void HarvestingATEFuses(void)
{
	/* Tensix column enablement */
	/* Aggregate ATE individual Tensix fuses into column-disable */
	uint32_t disabled_tensix_col = 0;

	disabled_tensix_col |= READ_FUNCTIONAL_EFUSE(ATE_TENSIX_ROW0_TEST_STATUS);
	disabled_tensix_col |= READ_FUNCTIONAL_EFUSE(ATE_TENSIX_ROW1_TEST_STATUS);
	disabled_tensix_col |= READ_FUNCTIONAL_EFUSE(ATE_TENSIX_ROW2_TEST_STATUS);
	disabled_tensix_col |= READ_FUNCTIONAL_EFUSE(ATE_TENSIX_ROW3_TEST_STATUS);
	disabled_tensix_col |= READ_FUNCTIONAL_EFUSE(ATE_TENSIX_ROW4_TEST_STATUS);
	disabled_tensix_col |= READ_FUNCTIONAL_EFUSE(ATE_TENSIX_ROW5_TEST_STATUS);
	disabled_tensix_col |= READ_FUNCTIONAL_EFUSE(ATE_TENSIX_ROW6_TEST_STATUS);
	disabled_tensix_col |= READ_FUNCTIONAL_EFUSE(ATE_TENSIX_ROW7_TEST_STATUS);
	disabled_tensix_col |= READ_FUNCTIONAL_EFUSE(ATE_TENSIX_ROW8_TEST_STATUS);
	disabled_tensix_col |= READ_FUNCTIONAL_EFUSE(ATE_TENSIX_ROW9_TEST_STATUS);
	tile_enable.tensix_col_enabled &= ~disabled_tensix_col;

	/* ETH tile enablement */
	tile_enable.eth_enabled &= ~READ_FUNCTIONAL_EFUSE(ATE_ETH_CTRL_TEST_STATUS);

	/* GDDR instance enablement */
	tile_enable.gddr_enabled &= ~READ_FUNCTIONAL_EFUSE(ATE_DDR_TEST_STATUS);

	/* L2CPU cluster enablement */
	uint32_t disabled_l2cpu = READ_FUNCTIONAL_EFUSE(ATE_RISCV_L2_TEST_STATUS);
	/* Original intention of the fuse was to have harvesting bits per core */
	/* Instead ATE is only fusing the first bit of each cluster to indicate */
	/* the entire cluster is harvested */
	/* Remap this to tile_enable.l2cpu_enabled, which is a bitmap of clusters */
	for (int i = 0; i < 4; i++) {
		if (disabled_l2cpu & BIT(i * 4)) {
			tile_enable.l2cpu_enabled &= ~BIT(i);
		}
	}

	uint32_t ate_pcie_fuse = READ_FUNCTIONAL_EFUSE(ATE_PCIE_SPEED_TEST);

	for (int i = 0; i < 2; i++) {
		uint8_t pcie_inst_fuse = FIELD_GET(0x3 << (i * 2), ate_pcie_fuse);

		if (pcie_inst_fuse == 3) {
			/* 0 = not fused (assume good) */
			/* 1 = reached Gen5 at ATE */
			/* 2 = reached Gen4 at ATE */
			/* 3 = failed */
			/* - SLT only screens out 3, so FW should adopt the same criterion */
			tile_enable.pcie_enabled &= ~BIT(i);
		}
	}
}

static uint32_t GetTensixDisableSLTMapV1(void)
{
	/* In fuse map version 1, the harvested columns were fused based on NOC0 X coordinate */
	/* However the fuse is only 16 bits wide, but there is a column where NOC0 X = 16 */
	/* Use the SLT binning fuse to determine if column x=16 should be harvested too */
	uint32_t slt_binning = READ_FUNCTIONAL_EFUSE(SLT_SLT_BINNING);
	/* Bits [15:8] = rebin */
	/* Bits [7:0] = original bin */
	if (FIELD_GET(0xFF00, slt_binning)) {
		slt_binning = FIELD_GET(0xFF00, slt_binning);
	} else {
		slt_binning = FIELD_GET(0xFF, slt_binning);
	}

	/* Determine if NOC0 x = 16 needs to be harvested */
	uint32_t harvested_columns = READ_FUNCTIONAL_EFUSE(SLT_HARVESTED_TENSIX_COLUMNS);
	uint8_t harvested_count = POPCOUNT(harvested_columns);

	if ((slt_binning == 3 || slt_binning == 4 || slt_binning == 5) && harvested_count < 2) {
		harvested_columns |= BIT(16);
	} else if ((slt_binning == 2) && harvested_count < 1) {
		harvested_columns |= BIT(16);
	}

	uint32_t harvested_columns_phys = 0;
	/* Convert back to physical */
	for (int x = 0; x < NOC_X_SIZE; x++) {
		if (harvested_columns & BIT(x)) {
			uint8_t phys_x = NocToTensixPhysX(x, 0);

			if (phys_x != 0xFF) {
				harvested_columns_phys |= BIT(phys_x);
			}
		}
	}
	return harvested_columns_phys;
}

static void HarvestingSLTFuses(void)
{
	uint32_t slt_fuse_map_version = READ_FUNCTIONAL_EFUSE(SLT_FUSE_MAP_VERSION);

	if ((READ_FUNCTIONAL_EFUSE(SLT_ATE_SLT_STATUS) & BIT(1)) == 0 ||
	    slt_fuse_map_version == 0) {
		/* SLT fuses invalid */
		return;
	}

	/* Tensix column enablement */
	if (slt_fuse_map_version == 1) {
		/* Workaround for SYS-1035 */
		tile_enable.tensix_col_enabled &= ~GetTensixDisableSLTMapV1();
	} else {
		tile_enable.tensix_col_enabled &=
			~READ_FUNCTIONAL_EFUSE(SLT_HARVESTED_TENSIX_COLUMNS);
	}

	/* ETH tile enablement */
	tile_enable.eth_enabled &= ~READ_FUNCTIONAL_EFUSE(SLT_ETH_CTRL_TEST_STATUS);

	/* GDDR tile enablement */
	uint8_t harvested_gddr = 0;

	harvested_gddr |= READ_FUNCTIONAL_EFUSE(SLT_DDR_TEST_STATUS_12G);
	harvested_gddr |= READ_FUNCTIONAL_EFUSE(SLT_DDR_TEST_STATUS_14G);
	harvested_gddr |= READ_FUNCTIONAL_EFUSE(SLT_DDR_TEST_STATUS_16G);

	/* Workaround for SYS-1065 */
	/* Ignore GDDR3 failures for fuse map <= v2 */
	if (slt_fuse_map_version <= 2) {
		harvested_gddr &= ~BIT(3);
	}

	tile_enable.gddr_enabled &= ~harvested_gddr;

	/* No SLT L2CPU or PCIe harvesting */
}

static int CalculateHarvesting(void)
{
	if (!IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	/* These are just the default values, taking some SPI parameters into account */
	/* This function needs to be completed with fuse reading and additional SPI parameters */

	/* Initial values, everything enabled */
	tile_enable.tensix_col_enabled = BIT_MASK(14);
	tile_enable.eth_enabled = BIT_MASK(14);
	tile_enable.eth5_serdes = true;
	tile_enable.eth8_serdes = true;
	tile_enable.eth_serdes_connected = BIT_MASK(12);
	tile_enable.gddr_enabled = BIT_MASK(8);
	tile_enable.l2cpu_enabled = BIT_MASK(4);
	tile_enable.pcie_enabled = BIT_MASK(2);

	if (tt_bh_fwtable_get_fw_table(fwtable_dev)->feature_enable.harvesting_en) {
		if (FusesValid()) {
			HarvestingATEFuses();
			HarvestingSLTFuses();
		}

		/* Eth handling */
		/* Only enable 2 of 3 in eth {4,5,6} */
		if (FIELD_GET(GENMASK(6, 4), tile_enable.eth_enabled) == BIT_MASK(3)) {
			tile_enable.eth_enabled &= ~BIT(5);
		}
		/* Only enable 2 of 3 in eth {7,8,9} */
		if (FIELD_GET(GENMASK(9, 7), tile_enable.eth_enabled) == BIT_MASK(3)) {
			tile_enable.eth_enabled &= ~BIT(8);
		}

		/* Soft harvesting for Tensix, ETH, GDDR, Tensix based on product spec */
		uint8_t disabled_tensix_cols = 14 - POPCOUNT(tile_enable.tensix_col_enabled);

		if (disabled_tensix_cols <
		    tt_bh_fwtable_get_fw_table(fwtable_dev)
			    ->product_spec_harvesting.tensix_col_disable_count) {
			uint8_t tensix_soft_disable =
				tt_bh_fwtable_get_fw_table(fwtable_dev)
					->product_spec_harvesting.tensix_col_disable_count -
				disabled_tensix_cols;

			for (int i = 13; i >= 0 && tensix_soft_disable > 0; i--) {
				if (IS_BIT_SET(tile_enable.tensix_col_enabled, i)) {
					WRITE_BIT(tile_enable.tensix_col_enabled, i, 0);
					tensix_soft_disable--;
				}
			}
		}

		if (tt_bh_fwtable_get_fw_table(fwtable_dev)->product_spec_harvesting.eth_disabled) {
			tile_enable.eth_enabled = 0;
		}

		uint8_t desired_gddr_count =
			8 - tt_bh_fwtable_get_fw_table(fwtable_dev)
				    ->product_spec_harvesting.dram_disable_count;

		if (POPCOUNT(tile_enable.gddr_enabled) > desired_gddr_count) {
			/* Only handle soft harvesting of one GDDR. Always choose GDDR3. */
			WRITE_BIT(tile_enable.gddr_enabled, 3, 0);
		}
	}

	/* PCIe and SERDES handling */
	tile_enable.pcie_usage[0] =
		tt_bh_fwtable_get_fw_table(fwtable_dev)->pci0_property_table.pcie_mode;
	if (tile_enable.pcie_usage[0] == FwTable_PciPropertyTable_PcieMode_DISABLED) {
		tile_enable.pcie_num_serdes[0] = 0;
	} else {
		tile_enable.pcie_num_serdes[0] = MIN(
			tt_bh_fwtable_get_fw_table(fwtable_dev)->pci0_property_table.num_serdes, 2);
		if (tile_enable.pcie_num_serdes[0] == 1) {
			tile_enable.eth_serdes_connected &= ~(BIT(0) | BIT(1));
		} else if (tile_enable.pcie_num_serdes[0] == 2) {
			tile_enable.eth_serdes_connected &= ~(BIT(0) | BIT(1) | BIT(2) | BIT(3));
		}
	}
	tile_enable.pcie_usage[1] =
		tt_bh_fwtable_get_fw_table(fwtable_dev)->pci1_property_table.pcie_mode;
	if (tile_enable.pcie_usage[1] == FwTable_PciPropertyTable_PcieMode_DISABLED) {
		tile_enable.pcie_num_serdes[1] = 0;
	} else {
		tile_enable.pcie_num_serdes[1] = MIN(
			tt_bh_fwtable_get_fw_table(fwtable_dev)->pci1_property_table.num_serdes, 2);
		if (tile_enable.pcie_num_serdes[1] == 1) {
			tile_enable.eth_serdes_connected &= ~(BIT(11) | BIT(10));
		} else if (tile_enable.pcie_num_serdes[1] == 2) {
			tile_enable.eth_serdes_connected &= ~(BIT(11) | BIT(10) | BIT(9) | BIT(8));
		}
	}

	return 0;
}
SYS_INIT_APP(CalculateHarvesting);
#endif /* CONFIG_BH_FWTABLE */
