/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/drivers/misc/tt_grendel_axi_filter.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <platform.h>
#include <spa_remap.h>

LOG_MODULE_REGISTER(tt_grendel_smc_filter, CONFIG_TT_GRENDEL_AXI_FILTER_LOG_LEVEL);

#define SMC_INBOUND_FILTER_NODE DT_NODELABEL(smc_inbound_filter)
#define SEP_SAFE_OFF_NODE       DT_NODELABEL(sep_safe_off)
#define SEP_SAFE_SIZE_NODE      DT_NODELABEL(sep_safe_size)
#define RAM0_NODE               DT_NODELABEL(ram0)

static const struct tt_grendel_axi_filter_flags smc_filter_host_flags = {
	.read = 1,
	.write = 1,
	.nonsecure = 1,
	.burst = 1,
};

/* Filter windows are chiplet-local AXI. Inputs must be SMC core-local aliases */
static int chiplet_local_addr(uint64_t addr, uint64_t *local)
{
	if (addr < SMC_CORE_LOCAL_ALIAS_BASE || addr > SMC_CORE_LOCAL_ALIAS_END) {
		LOG_ERR("Address 0x%llx is not an SMC core-local alias", addr);
		return -EINVAL;
	}

	*local = (addr - SMC_CORE_LOCAL_ALIAS_BASE) + KER_SMC_LOCAL_BASE;
	return 0;
}

static int tt_grendel_smc_filter_pre_pcie_init(void)
{
	const struct device *filter = DEVICE_DT_GET(SMC_INBOUND_FILTER_NODE);
	uint64_t sep_start;
	uint64_t sep_end;
	uint32_t sep_off;
	uint32_t sep_size;
	int ret;

	if (!device_is_ready(filter)) {
		LOG_ERR("SMC inbound filter is not ready");
		return -ENODEV;
	}

	sep_off = sys_read32(DT_REG_ADDR(SEP_SAFE_OFF_NODE)) & GENMASK(13, 0);
	sep_size = sys_read32(DT_REG_ADDR(SEP_SAFE_SIZE_NODE));
	if (sep_size == 0 || sep_off > SMC_CPU_SPM_MEMORY_MEM_SIZE ||
	    sep_size > SMC_CPU_SPM_MEMORY_MEM_SIZE - sep_off) {
		LOG_ERR("Invalid SEP-safe SRAM range: offset 0x%x size 0x%x", sep_off, sep_size);
		return -EINVAL;
	}

	ret = chiplet_local_addr(SMC_CPU_SPM_MEMORY_MEM_BASE_ADDR + sep_off, &sep_start);
	if (ret < 0) {
		return ret;
	}

	ret = chiplet_local_addr(SMC_CPU_SPM_MEMORY_MEM_BASE_ADDR + sep_off + sep_size - 1,
				 &sep_end);
	if (ret < 0) {
		return ret;
	}

	ret = tt_grendel_axi_filter_enable(filter, 1, sep_start, sep_end, smc_filter_host_flags);
	if (ret < 0) {
		LOG_ERR("Failed to enable SEP-safe SRAM filter: %d", ret);
	}

	return ret;
}

static int tt_grendel_smc_filter_post_pcie_init(void)
{
	const struct device *filter = DEVICE_DT_GET(SMC_INBOUND_FILTER_NODE);
	uint64_t bun2_start;
	uint64_t bun2_end;
	int ret;

	if (!device_is_ready(filter)) {
		LOG_ERR("SMC inbound filter is not ready");
		return -ENODEV;
	}

	/* Staging runs from the BUN2 base up to (not into) BL0P5's ram0 reservation. */
	ret = chiplet_local_addr(TT_BUN2_STAGING_AREA_ADDR, &bun2_start);
	if (ret < 0) {
		return ret;
	}

	ret = chiplet_local_addr(DT_REG_ADDR_U64(RAM0_NODE) - 1, &bun2_end);
	if (ret < 0) {
		return ret;
	}

	ret = tt_grendel_axi_filter_enable(filter, 0, bun2_start, bun2_end, smc_filter_host_flags);
	if (ret < 0) {
		LOG_ERR("Failed to enable BUN2 staging filter: %d", ret);
	}

	return ret;
}

SYS_INIT(tt_grendel_smc_filter_pre_pcie_init, PRE_KERNEL_2, 0);

SYS_INIT(tt_grendel_smc_filter_post_pcie_init, POST_KERNEL, 0);
