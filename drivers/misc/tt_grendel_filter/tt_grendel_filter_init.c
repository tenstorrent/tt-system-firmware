/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <tenstorrent/uart_tt_virt.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/drivers/misc/tt_grendel_filter.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <firewall.h>
#include <platform.h>
#include <soc.h>
#include <spa_remap.h>

LOG_MODULE_DECLARE(tt_grendel_filter, CONFIG_TT_GRENDEL_FILTER_LOG_LEVEL);

#define SEP_SAFE_OFF_NODE  DT_NODELABEL(sep_safe_off)
#define SEP_SAFE_SIZE_NODE DT_NODELABEL(sep_safe_size)
#define RAM0_NODE          DT_CHOSEN(zephyr_sram)
#define VUART_NODE         DT_CHOSEN(zephyr_console)

#define BUN2_FILTER_ENTRY     0
#define SEP_SAFE_FILTER_ENTRY 1
#define SCRATCH_FILTER_ENTRY  2
#define VUART_FILTER_ENTRY    3

#define SMC_SCRATCH_COUNT 16
#define SMC_SCRATCH_END                                                                            \
	(SCRATCH_REG_BASE + ((SMC_SCRATCH_COUNT - 1) * 2 * sizeof(uint32_t)) + sizeof(uint32_t) - 1)
#define VUART_SIZE                                                                                 \
	ROUND_UP(sizeof(struct tt_vuart) + DT_PROP(VUART_NODE, tx_cap) +                           \
			 DT_PROP(VUART_NODE, rx_cap),                                              \
		 sizeof(uint32_t))

BUILD_ASSERT(DT_NODE_HAS_COMPAT(VUART_NODE, tenstorrent_vuart),
	     "zephyr,console must be a Tenstorrent VUART");

static const struct tt_grendel_filter_flags smc_filter_host_flags = {
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

static int enable_host_window(uint32_t entry, uint64_t start, uint64_t end, const char *name)
{
	uint64_t local_start;
	uint64_t local_end;
	int ret;

	ret = chiplet_local_addr(start, &local_start);
	if (ret < 0) {
		return ret;
	}

	ret = chiplet_local_addr(end, &local_end);
	if (ret < 0) {
		return ret;
	}

	ret = tt_grendel_filter_enable(SMC_INBOUND, entry, 0, local_start, local_end,
				       smc_filter_host_flags);
	if (ret < 0) {
		LOG_ERR("Failed to enable %s filter: %d", name, ret);
		return ret;
	}

	LOG_INF("%s window [0x%llx, 0x%llx]", name, (unsigned long long)local_start,
		(unsigned long long)local_end);
	return 0;
}

static int tt_grendel_smc_filter_pre_pcie_init(void)
{
	const struct device *vuart = DEVICE_DT_GET(VUART_NODE);
	uintptr_t vuart_start;
	uint32_t sep_off;
	uint32_t sep_size;
	int ret;

	if (!device_is_ready(vuart)) {
		LOG_ERR("VUART is not ready");
		return -ENODEV;
	}

	ret = enable_host_window(SCRATCH_FILTER_ENTRY, SCRATCH_REG_BASE, SMC_SCRATCH_END,
				 "scratch");
	if (ret < 0) {
		return ret;
	}

	vuart_start = (uintptr_t)uart_tt_virt_get(vuart);
	ret = enable_host_window(VUART_FILTER_ENTRY, vuart_start, vuart_start + VUART_SIZE - 1,
				 "VUART");
	if (ret < 0) {
		return ret;
	}

	sep_off = sys_read32(DT_REG_ADDR(SEP_SAFE_OFF_NODE)) & GENMASK(13, 0);
	sep_size = sys_read32(DT_REG_ADDR(SEP_SAFE_SIZE_NODE));
	if (sep_size == 0 || sep_off > SMC_CPU_SPM_MEMORY_MEM_SIZE ||
	    sep_size > SMC_CPU_SPM_MEMORY_MEM_SIZE - sep_off) {
		LOG_ERR("Invalid SEP-safe SRAM range: offset 0x%x size 0x%x", sep_off, sep_size);
		return -EINVAL;
	}

	return enable_host_window(SEP_SAFE_FILTER_ENTRY, SMC_CPU_SPM_MEMORY_MEM_BASE_ADDR + sep_off,
				  SMC_CPU_SPM_MEMORY_MEM_BASE_ADDR + sep_off + sep_size - 1,
				  "SEP-safe SRAM");
}

static int tt_grendel_smc_filter_post_pcie_init(void)
{
	uint64_t bun2_end = DT_REG_ADDR_U64(RAM0_NODE) - 1;

	/* Images using the full ram0 region do not reserve BUN2 staging below themselves. */
	if (bun2_end < TT_BUN2_STAGING_AREA_ADDR) {
		return 0;
	}

	/* Staging runs from the BUN2 base up to (not into) BL0P5's ram0 reservation. */
	return enable_host_window(BUN2_FILTER_ENTRY, TT_BUN2_STAGING_AREA_ADDR, bun2_end,
				  "BUN2 staging");
}

SYS_INIT(tt_grendel_smc_filter_pre_pcie_init, PRE_KERNEL_2, 0);

SYS_INIT(tt_grendel_smc_filter_post_pcie_init, POST_KERNEL, 0);
