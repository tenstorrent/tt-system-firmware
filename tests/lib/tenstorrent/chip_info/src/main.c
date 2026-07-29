/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for the recovery (static no-SPI) chip-info backend.
 *
 * SMC recovery has no SPI firmware tables to read, so chip_info_static.c
 * synthesizes conservative defaults.
 */

#include <stdint.h>

#include <zephyr/ztest.h>

#include "chip_info.h"
#include "pcie.h"

/* The Tenstorrent PCI vendor ID. Recovery must program this even without SPI
 * tables, otherwise the device does not enumerate as Tenstorrent and host
 * tooling cannot re-flash it.
 */
#define TT_PCIE_VENDOR_ID 0x1e52

ZTEST(chip_info_static, test_vendor_id_is_tenstorrent)
{
	zassert_equal(bh_chip_info_vendor_id(), TT_PCIE_VENDOR_ID,
		      "recovery must program the Tenstorrent PCI vendor ID (0x1e52)");
}

ZTEST(chip_info_static, test_board_identity_defaults)
{
	zassert_equal(bh_chip_info_board_id(), 0, "recovery has no SPI board_id to report");
	zassert_equal(bh_chip_info_additional_board_power(), 0,
		      "recovery reports no extra board power budget");
}

ZTEST(chip_info_static, test_features_disabled)
{
	/* native_sim is not a Galaxy/UBB board revision. */
	zassert_false(bh_chip_info_is_ubb(), "default board revision is not UBB");
	zassert_false(bh_chip_info_feature_cg_en(), "clock gating defaults off in recovery");
	zassert_false(bh_chip_info_feature_noc_translation_en(),
		      "NOC translation defaults off in recovery");
}

ZTEST(chip_info_static, test_pci_property_defaults)
{
	struct bh_pci_property prop;

	bh_chip_info_pci_property(0, &prop);

	zassert_equal(prop.pcie_mode, BH_PCIE_MODE_EP, "recovery brings PCIe up as an endpoint");
	/* native_sim is not a P300/Galaxy revision, so two serdes are expected. */
	zassert_equal(prop.num_serdes, 2, "non-P300/Galaxy boards use two serdes");
	zassert_equal(prop.max_pcie_speed, 0, "recovery leaves PCIe speed unconstrained");
	zassert_equal(prop.pcie_bar0_size, PCIE_BAR0_SIZE_DEFAULT_MB, "BAR0 must match pcie.c");
	zassert_equal(prop.pcie_bar2_size, PCIE_BAR2_SIZE_DEFAULT_MB, "BAR2 must match pcie.c");
	zassert_equal(prop.pcie_bar4_size, PCIE_BAR4_SIZE_DEFAULT_MB, "BAR4 must match pcie.c");
}

ZTEST(chip_info_static, test_pci_property_instance_independent)
{
	struct bh_pci_property prop0;
	struct bh_pci_property prop1;

	bh_chip_info_pci_property(0, &prop0);
	bh_chip_info_pci_property(1, &prop1);

	/* Recovery ignores the instance argument and synthesizes identical defaults. */
	zassert_mem_equal(&prop0, &prop1, sizeof(prop0),
			  "both PCIe instances synthesize identical recovery defaults");
}

ZTEST_SUITE(chip_info_static, NULL, NULL, NULL, NULL, NULL);
