/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SiVal headers only. Do not include Zephyr from this file.
 */

#include "sival_addrs.h"

#include <firewall.h>
#include <platform.h>

uint64_t tt_sival_inbound_filter_base(void)
{
	return SMC_CPU_SMC_INBOUND_FILTER_CTRL_0_REG_MAP_BASE_ADDR;
}

uint32_t tt_sival_smc_inbound(void)
{
	return SMC_INBOUND;
}

uint32_t tt_sival_filter_stride(void)
{
	return FIREWALL_FILTER_SIZE;
}

uint32_t tt_sival_filter_config_offset(void)
{
	return FIREWALL_FILTER_CONFIG_OFFSET;
}

uint32_t tt_sival_filter_start_offset(void)
{
	return FIREWALL_START_ADDR_OFFSET;
}

uint32_t tt_sival_filter_end_offset(void)
{
	return FIREWALL_END_ADDR_OFFSET;
}
