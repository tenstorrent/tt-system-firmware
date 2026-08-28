/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_grendel_axi_filter

#include <errno.h>
#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/drivers/misc/tt_grendel_axi_filter.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define FILTER_CONFIG_OFFSET 0x0
#define FILTER_START_OFFSET  0x8
#define FILTER_END_OFFSET    0x10

#define FILTER_ADDRESS_MASK GENMASK64(55, 0)

struct filter_config {
	uint64_t read_en: 1;
	uint64_t write_en: 1;
	uint64_t rsvd_0: 2;
	uint64_t addr_mode: 1;
	uint64_t rsvd_1: 3;
	uint64_t allow_ns: 1;
	uint64_t rsvd_2: 3;
	uint64_t data_bus_width: 3;
	uint64_t rsvd_3: 1;
	uint64_t src_id: 4;
	uint64_t group_id: 4;
	uint64_t allow_burst: 1;
	uint64_t rsvd_4: 38;
	uint64_t locked: 1;
};

union filter_config_reg {
	uint64_t val;
	struct filter_config f;
};

BUILD_ASSERT(sizeof(union filter_config_reg) == 8);

struct tt_grendel_axi_filter_config {
	uintptr_t base;
	size_t size;
	size_t stride;
};

static bool filter_entry_valid(const struct tt_grendel_axi_filter_config *config, uint32_t entry)
{
	return entry < (config->size / config->stride);
}

int tt_grendel_axi_filter_enable(const struct device *dev, uint32_t entry, uint64_t start,
				 uint64_t end, struct tt_grendel_axi_filter_flags flags)
{
	const struct tt_grendel_axi_filter_config *config = dev->config;
	union filter_config_reg filter_config = {0};
	uintptr_t entry_addr;

	if (!filter_entry_valid(config, entry) || start > end ||
	    (start & ~FILTER_ADDRESS_MASK) != 0 || (end & ~FILTER_ADDRESS_MASK) != 0 ||
	    (flags.read == 0 && flags.write == 0)) {
		return -EINVAL;
	}

	filter_config.f.read_en = flags.read;
	filter_config.f.write_en = flags.write;
	filter_config.f.addr_mode = 1;
	filter_config.f.allow_ns = flags.nonsecure;
	filter_config.f.allow_burst = flags.burst;

	entry_addr = config->base + (entry * config->stride);
	sys_write64(start, entry_addr + FILTER_START_OFFSET);
	sys_write64(end, entry_addr + FILTER_END_OFFSET);
	sys_write64(filter_config.val, entry_addr + FILTER_CONFIG_OFFSET);

	return 0;
}

int tt_grendel_axi_filter_disable(const struct device *dev, uint32_t entry)
{
	const struct tt_grendel_axi_filter_config *config = dev->config;
	uintptr_t entry_addr;

	if (!filter_entry_valid(config, entry)) {
		return -EINVAL;
	}

	entry_addr = config->base + (entry * config->stride);
	sys_write64(0, entry_addr + FILTER_CONFIG_OFFSET);
	sys_write64(0, entry_addr + FILTER_START_OFFSET);
	sys_write64(0, entry_addr + FILTER_END_OFFSET);

	return 0;
}

#define TT_GRENDEL_AXI_FILTER_DEFINE(inst)                                                         \
	BUILD_ASSERT(DT_INST_PROP(inst, entry_stride) >= 0x18,                                     \
		     "AXI filter entry stride is too small");                                      \
	BUILD_ASSERT(DT_INST_REG_SIZE(inst) % DT_INST_PROP(inst, entry_stride) == 0,               \
		     "AXI filter region is not a whole number of entries");                        \
	static const struct tt_grendel_axi_filter_config tt_grendel_axi_filter_config_##inst = {   \
		.base = DT_INST_REG_ADDR_U64(inst),                                                \
		.size = DT_INST_REG_SIZE(inst),                                                    \
		.stride = DT_INST_PROP(inst, entry_stride),                                        \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, NULL, &tt_grendel_axi_filter_config_##inst,        \
			      PRE_KERNEL_1, 0, NULL);

DT_INST_FOREACH_STATUS_OKAY(TT_GRENDEL_AXI_FILTER_DEFINE)
