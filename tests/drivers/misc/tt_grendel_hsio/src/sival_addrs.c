/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SiVal headers only. Do not include Zephyr from this file.
 */

#include "sival_addrs.h"

#include <basic_init.h>
#include <firewall.h>
#include <platform.h>

uint32_t tt_sival_ker_rst_hsio0(void)
{
	return KER_RST_HSIO0;
}

uint32_t tt_sival_smc_cold_reset_n_addr(void)
{
	return SMC_CPU_RESET_UNIT_SS_COLD_RESET_N_REG_ADDR;
}

uint32_t tt_sival_hsio0_cold_reset_n_addr(void)
{
	return HSIO_TILE_0_HSIO_PLL_RESET_CTRL_SS_COLD_RESET_N_REG_ADDR;
}

uint32_t tt_sival_hsio0_ag_mux_addr(void)
{
	return HSIO_TILE_0_HSIO_PLL_RESET_CTRL_AG_MUX_SELECT_REG_ADDR;
}

uint64_t tt_sival_smn_hsio2hsio_filter_base(void)
{
	return SMN0_BASE_ADDR + SMN_HSIO2HSIO_FIREWALL_OFFSET + FIREWALL_FILTER_CONFIG_OFFSET;
}

uint64_t tt_sival_smn_hsio2smn_filter_base(void)
{
	return SMN0_BASE_ADDR + SMN_HSIO2SMN_FIREWALL_OFFSET + FIREWALL_FILTER_CONFIG_OFFSET;
}

uint64_t tt_sival_hsio_noc2axi_filter_base(void)
{
	return HSIO0_BASE_ADDR + HSIO_NOC2AXI_FIREWALL_OFFSET + FIREWALL_FILTER_CONFIG_OFFSET;
}

uint64_t tt_sival_hsio_pcie_filter_base(void)
{
	return HSIO0_BASE_ADDR + HSIO_PCIE_FIREWALL_OFFSET + FIREWALL_FILTER_CONFIG_OFFSET;
}

uint32_t tt_sival_filter_stride(void)
{
	return FIREWALL_FILTER_SIZE;
}

uint32_t tt_sival_filter_start_offset(void)
{
	return FIREWALL_START_ADDR_OFFSET;
}

uint32_t tt_sival_filter_end_offset(void)
{
	return FIREWALL_END_ADDR_OFFSET;
}

uint64_t tt_sival_firewall_addr_mask(void)
{
	return FIREWALL_ADDR_MASK;
}
