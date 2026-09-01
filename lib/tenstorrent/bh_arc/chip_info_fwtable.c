/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Mission (bh_arc_mission) backend for the chip-info service: sources every
 * value from the SPI firmware tables via the bh_fwtable driver.
 */

#include "chip_info.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/bh_fwtable.h>
#include <zephyr/sys/util.h>

static const struct device *const fwtable_dev = DEVICE_DT_GET(DT_NODELABEL(fwtable));

/* bh_chip_info_pci_property() casts the protobuf pcie_mode straight to
 * bh_pcie_mode_t, so the two enums must stay numerically identical.
 */
BUILD_ASSERT((int)BH_PCIE_MODE_DISABLED == FwTable_PciPropertyTable_PcieMode_DISABLED);
BUILD_ASSERT((int)BH_PCIE_MODE_EP == FwTable_PciPropertyTable_PcieMode_EP);
BUILD_ASSERT((int)BH_PCIE_MODE_RP == FwTable_PciPropertyTable_PcieMode_RP);

bool bh_chip_info_is_ubb(void)
{
	uint32_t board_type = tt_bh_fwtable_get_board_type(fwtable_dev);

	return board_type == BOARDTYPE_UBB || board_type == BOARDTYPE_GALAXY_BIN6;
}

uint32_t bh_chip_info_additional_board_power(void)
{
	return tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.additional_board_power;
}

uint64_t bh_chip_info_board_id(void)
{
	return tt_bh_fwtable_get_read_only_table(fwtable_dev)->board_id;
}

uint32_t bh_chip_info_vendor_id(void)
{
	return tt_bh_fwtable_get_read_only_table(fwtable_dev)->vendor_id;
}

void bh_chip_info_pci_property(uint8_t pcie_inst, struct bh_pci_property *out)
{
	const FwTable *fw_table = tt_bh_fwtable_get_fw_table(fwtable_dev);
	const FwTable_PciPropertyTable *t =
		(pcie_inst == 0) ? &fw_table->pci0_property_table : &fw_table->pci1_property_table;

	*out = (struct bh_pci_property){
		.pcie_mode = (bh_pcie_mode_t)t->pcie_mode,
		.num_serdes = t->num_serdes,
		.max_pcie_speed = t->max_pcie_speed,
		.pcie_bar0_size = t->pcie_bar0_size,
		.pcie_bar2_size = t->pcie_bar2_size,
		.pcie_bar4_size = t->pcie_bar4_size,
	};
}

bool bh_chip_info_feature_cg_en(void)
{
	return tt_bh_fwtable_get_fw_table(fwtable_dev)->feature_enable.cg_en;
}

bool bh_chip_info_feature_noc_translation_en(void)
{
	return tt_bh_fwtable_get_fw_table(fwtable_dev)->feature_enable.noc_translation_en;
}
