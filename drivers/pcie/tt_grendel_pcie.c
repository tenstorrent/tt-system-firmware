/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_grendel_pcie

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "pcie_init.h"

#include <basic_init.h>
#include <platform.h>

LOG_MODULE_REGISTER(tt_grendel_pcie, CONFIG_TT_GRENDEL_PCIE_LOG_LEVEL);

struct tt_grendel_pcie_config {
	uint64_t bar4_size;
};

/*
 * SiVal pcie_init_config() ATU (used until Arch Spec 6.4 is checked):
 *   BAR0 -> APP0
 *   BAR2 -> SYSIN0
 *   BAR4 -> SPA 0x0001200000000
 * Boot spec Table 149 differs (BAR0 SYSIN0, BAR2 APPIN0 mailbox, BAR4 APPIN1
 * DRAM). Do not override the lib maps here.
 *
 * pcie_load_firmware() currently copies baked iccm/dccm arrays inside
 * libdriver_pcie.a. SiVal will retarget that symbol to a staged ICCM+DCCM
 * SERDES bin; keep calling pcie_load_firmware() (via the state machine).
 */

static int tt_grendel_pcie_init(const struct device *dev)
{
	pcie_link_status_t link;
	grendel_err_t err;

#ifdef CONFIG_TT_GRENDEL_PCIE_HW_INIT
	const struct tt_grendel_pcie_config *config = dev->config;
	pcie_config_t cfg;

	pcie_init_config(&cfg);
	cfg.bars[2].size = config->bar4_size;

	/* Blocking SM: RESET … LINK_L0 … ENUM_READY (emul skips host enum). */
	err = pcie_init_with_state_machine(&cfg, NULL);
	if (err != GRENDEL_ERR_OK) {
		/* TODO: Bootspec S14.4 retry or invoke warm reset */
		LOG_ERR("pcie_init_with_state_machine failed: %d", err);
		return -EIO;
	}

	/*
	 * TLB/BAR/ATU map BARs to SPA. Table 17 inbound remappers rewrite SPA
	 * to chiplet-local AXI and are not part of the state machine.
	 */
	err = set_ker_pcie_remappers(PCIE);
	if (err != GRENDEL_ERR_OK) {
		LOG_ERR("set_ker_pcie_remappers failed: %d", err);
		return -EIO;
	}
#else
	ARG_UNUSED(dev);
#endif

	/*
	 * Always verify the link, including images that did not train it
	 * (HW_INIT=n). Bootloader and mission must confirm PCIe is
	 * functional. Action on failure (retry / warm reset) is TBD.
	 */
	err = pcie_get_link_status(&link);
	if (err != GRENDEL_ERR_OK) {
		LOG_ERR("pcie_get_link_status failed: %d", err);
		return -EIO;
	}

	LOG_INF("link up ltssm=%u gen=%u x%u", link.ltssm_state, link.link_speed, link.link_width);
	return 0;
}

#define TT_GRENDEL_PCIE_DEFINE(inst)                                                               \
	static const struct tt_grendel_pcie_config config_##inst = {                               \
		/* bar4-size is two 32-bit cells (high, then low). */                              \
		.bar4_size = ((uint64_t)DT_INST_PROP_BY_IDX(inst, bar4_size, 0) << 32) |           \
			     DT_INST_PROP_BY_IDX(inst, bar4_size, 1),                              \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, tt_grendel_pcie_init, NULL, NULL, &config_##inst,              \
			      PRE_KERNEL_2, CONFIG_TT_GRENDEL_PCIE_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(TT_GRENDEL_PCIE_DEFINE)
