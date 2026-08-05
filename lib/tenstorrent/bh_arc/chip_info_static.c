/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Recovery (bh_arc_recovery) backend for the chip-info service.
 *
 * SMC recovery must come up far enough for the host to re-flash the chip even
 * when the SPI firmware tables are missing or corrupt, so this backend never
 * reads SPI. It returns conservative, board-revision-derived defaults instead.
 */

#include "chip_info.h"
#include "pcie.h"

#include <zephyr/sys/util.h>

/* Serdes count by board: P300 variants and UBB (Galaxy) use 1, P100/P150 use 2. */
#if defined(CONFIG_BOARD_REVISION_P300A) || defined(CONFIG_BOARD_REVISION_P300B) ||                \
	defined(CONFIG_BOARD_REVISION_P300C) || defined(CONFIG_BOARD_REVISION_GALAXY) ||           \
	defined(CONFIG_BOARD_REVISION_GALAXY_REVC) || defined(CONFIG_BOARD_REVISION_GALAXY_BIN6)
#define RECOVERY_NUM_SERDES 1
#else
#define RECOVERY_NUM_SERDES 2
#endif

bool bh_chip_info_is_ubb(void)
{
#if defined(CONFIG_BOARD_REVISION_GALAXY) || defined(CONFIG_BOARD_REVISION_GALAXY_REVC) ||         \
	defined(CONFIG_BOARD_REVISION_GALAXY_BIN6)
	return true;
#else
	return false;
#endif
}

uint32_t bh_chip_info_additional_board_power(void)
{
	return 0;
}

uint64_t bh_chip_info_board_id(void)
{
	return 0;
}

uint32_t bh_chip_info_vendor_id(void)
{
	/* Tenstorrent PCI vendor ID. Recovery has no SPI tables to read it from, but
	 * it must still program the real VID so the device enumerates as Tenstorrent
	 * and host tooling can re-flash it.
	 */
	return 0x1e52;
}

void bh_chip_info_pci_property(uint8_t pcie_inst, struct bh_pci_property *out)
{
	ARG_UNUSED(pcie_inst);

	*out = (struct bh_pci_property){
		.pcie_mode = BH_PCIE_MODE_EP,
		.num_serdes = RECOVERY_NUM_SERDES,
		.max_pcie_speed = 0,
		.pcie_bar0_size = PCIE_BAR0_SIZE_DEFAULT_MB,
		.pcie_bar2_size = PCIE_BAR2_SIZE_DEFAULT_MB,
		.pcie_bar4_size = PCIE_BAR4_SIZE_DEFAULT_MB,
	};
}

bool bh_chip_info_feature_cg_en(void)
{
	return false;
}

bool bh_chip_info_feature_noc_translation_en(void)
{
	return false;
}
