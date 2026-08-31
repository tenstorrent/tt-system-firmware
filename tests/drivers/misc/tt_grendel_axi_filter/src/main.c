/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/misc/tt_grendel_axi_filter.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#define FILTER_STRIDE        0x20
#define FILTER_ENTRIES       2
#define FILTER_CONFIG_OFFSET 0x0
#define FILTER_START_OFFSET  0x8
#define FILTER_END_OFFSET    0x10

#define FILTER_CONFIG_READ_EN     BIT64(0)
#define FILTER_CONFIG_WRITE_EN    BIT64(1)
#define FILTER_CONFIG_ADDR_MODE   BIT64(4)
#define FILTER_CONFIG_ALLOW_NS    BIT64(8)
#define FILTER_CONFIG_ALLOW_BURST BIT64(24)

static uint8_t mmio[FILTER_ENTRIES * FILTER_STRIDE] __aligned(8);

/* Must match the driver device config layout. */
struct tt_grendel_axi_filter_config {
	uintptr_t base;
	size_t size;
	size_t stride;
};

static const struct tt_grendel_axi_filter_config filter_cfg = {
	.base = (uintptr_t)mmio,
	.size = sizeof(mmio),
	.stride = FILTER_STRIDE,
};

static const struct device filter_dev = {
	.config = &filter_cfg,
};

static uint64_t read_entry_reg(uint32_t entry, size_t offset)
{
	return sys_read64((uintptr_t)&mmio[entry * FILTER_STRIDE] + offset);
}

static void before(void *f)
{
	ARG_UNUSED(f);
	memset(mmio, 0xaa, sizeof(mmio));
}

ZTEST(tt_grendel_axi_filter, test_enable_programs_entry)
{
	struct tt_grendel_axi_filter_flags flags = {
		.read = 1,
		.write = 1,
		.nonsecure = 1,
		.burst = 1,
	};
	int ret = tt_grendel_axi_filter_enable(&filter_dev, 0, 0x08018000, 0x08037fff, flags);

	zassert_equal(ret, 0);
	zassert_equal(read_entry_reg(0, FILTER_START_OFFSET), 0x08018000);
	zassert_equal(read_entry_reg(0, FILTER_END_OFFSET), 0x08037fff);
	zassert_equal(read_entry_reg(0, FILTER_CONFIG_OFFSET),
		      FILTER_CONFIG_READ_EN | FILTER_CONFIG_WRITE_EN | FILTER_CONFIG_ADDR_MODE |
			      FILTER_CONFIG_ALLOW_NS | FILTER_CONFIG_ALLOW_BURST);
}

ZTEST(tt_grendel_axi_filter, test_enable_read_only)
{
	struct tt_grendel_axi_filter_flags flags = {
		.read = 1,
	};
	int ret = tt_grendel_axi_filter_enable(&filter_dev, 0, 0x1000, 0x1fff, flags);

	zassert_equal(ret, 0);
	zassert_equal(read_entry_reg(0, FILTER_CONFIG_OFFSET),
		      FILTER_CONFIG_READ_EN | FILTER_CONFIG_ADDR_MODE);
}

ZTEST(tt_grendel_axi_filter, test_enable_equal_range)
{
	struct tt_grendel_axi_filter_flags flags = {
		.write = 1,
	};
	int ret = tt_grendel_axi_filter_enable(&filter_dev, 0, 0x2000, 0x2000, flags);

	zassert_equal(ret, 0);
	zassert_equal(read_entry_reg(0, FILTER_START_OFFSET), 0x2000);
	zassert_equal(read_entry_reg(0, FILTER_END_OFFSET), 0x2000);
}

ZTEST(tt_grendel_axi_filter, test_enable_does_not_touch_other_entry)
{
	struct tt_grendel_axi_filter_flags flags = {
		.read = 1,
		.write = 1,
	};
	int ret = tt_grendel_axi_filter_enable(&filter_dev, 0, 0x0, 0xff, flags);

	zassert_equal(ret, 0);
	zassert_equal(read_entry_reg(1, FILTER_CONFIG_OFFSET), 0xaaaaaaaaaaaaaaaaULL);
	zassert_equal(read_entry_reg(1, FILTER_START_OFFSET), 0xaaaaaaaaaaaaaaaaULL);
	zassert_equal(read_entry_reg(1, FILTER_END_OFFSET), 0xaaaaaaaaaaaaaaaaULL);
}

ZTEST(tt_grendel_axi_filter, test_disable_clears_entry)
{
	struct tt_grendel_axi_filter_flags flags = {
		.read = 1,
		.write = 1,
		.burst = 1,
	};
	int ret;

	ret = tt_grendel_axi_filter_enable(&filter_dev, 1, 0x10, 0x1f, flags);
	zassert_equal(ret, 0);

	ret = tt_grendel_axi_filter_disable(&filter_dev, 1);
	zassert_equal(ret, 0);
	zassert_equal(read_entry_reg(1, FILTER_CONFIG_OFFSET), 0);
	zassert_equal(read_entry_reg(1, FILTER_START_OFFSET), 0);
	zassert_equal(read_entry_reg(1, FILTER_END_OFFSET), 0);
}

ZTEST(tt_grendel_axi_filter, test_enable_rejects_bad_args)
{
	struct tt_grendel_axi_filter_flags rw = {
		.read = 1,
		.write = 1,
	};
	struct tt_grendel_axi_filter_flags none = {0};
	uint8_t snapshot[sizeof(mmio)];

	memcpy(snapshot, mmio, sizeof(mmio));

	zassert_equal(tt_grendel_axi_filter_enable(&filter_dev, FILTER_ENTRIES, 0, 1, rw), -EINVAL);
	zassert_equal(tt_grendel_axi_filter_enable(&filter_dev, 0, 0x20, 0x10, rw), -EINVAL);
	zassert_equal(tt_grendel_axi_filter_enable(&filter_dev, 0, BIT64(56), BIT64(56), rw),
		      -EINVAL);
	zassert_equal(tt_grendel_axi_filter_enable(&filter_dev, 0, 0, 1, none), -EINVAL);
	zassert_mem_equal(mmio, snapshot, sizeof(mmio));
}

ZTEST(tt_grendel_axi_filter, test_disable_rejects_bad_entry)
{
	uint8_t snapshot[sizeof(mmio)];

	memcpy(snapshot, mmio, sizeof(mmio));
	zassert_equal(tt_grendel_axi_filter_disable(&filter_dev, FILTER_ENTRIES), -EINVAL);
	zassert_mem_equal(mmio, snapshot, sizeof(mmio));
}

ZTEST_SUITE(tt_grendel_axi_filter, NULL, NULL, before, NULL, NULL);
