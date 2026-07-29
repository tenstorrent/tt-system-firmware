/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TT_BH_ARC_CHIP_INFO_H
#define TT_BH_ARC_CHIP_INFO_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Abstracts chip-configuration values for the recovery and mission bh_arc.
 * Mission mode reads values from fwtable while recovery returns hardcoded values.
 */

/* PCIe operating mode. Values intentionally match the protobuf
 * FwTable_PciPropertyTable_PcieMode enum so the fwtable backend can pass them
 * through unchanged.
 */
typedef enum {
	BH_PCIE_MODE_DISABLED = 0,
	BH_PCIE_MODE_EP = 1,
	BH_PCIE_MODE_RP = 2,
} bh_pcie_mode_t;

struct bh_pci_property {
	bh_pcie_mode_t pcie_mode;
	uint32_t num_serdes;
	uint32_t max_pcie_speed;
	uint32_t pcie_bar0_size;
	uint32_t pcie_bar2_size;
	uint32_t pcie_bar4_size;
	uint32_t gen3_eq_pset_req_vec;
	uint8_t gen3_eq_fb_mode;
};

/* True if this is a UBB (Galaxy) board. Used to skip DMC cable-fault checks.
 * Exposed as a semantic predicate rather than a raw board-type byte so that
 * recovery code never needs to include the bh_fwtable / protobuf headers.
 */
bool bh_chip_info_is_ubb(void);

/* Extra board power budget, in W, added to host-reported power. */
uint32_t bh_chip_info_additional_board_power(void);

/* Read-only board identity. */
uint64_t bh_chip_info_board_id(void);
uint32_t bh_chip_info_vendor_id(void);

/* PCIe property table for instance @p pcie_inst (0 or 1). */
void bh_chip_info_pci_property(uint8_t pcie_inst, struct bh_pci_property *out);

/* Feature-enable bits. */
bool bh_chip_info_feature_cg_en(void);
bool bh_chip_info_feature_noc_translation_en(void);

#endif /* TT_BH_ARC_CHIP_INFO_H */
