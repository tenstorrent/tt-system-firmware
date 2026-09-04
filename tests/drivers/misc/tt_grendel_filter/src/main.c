/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/misc/tt_grendel_filter.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "regs.h"
#include "sival_addrs.h"

void tt_grendel_reg_stub_reset(void);

#define FILTER_STRIDE        tt_sival_filter_stride()
#define FILTER_CONFIG_OFFSET tt_sival_filter_config_offset()
#define FILTER_START_OFFSET  tt_sival_filter_start_offset()
#define FILTER_END_OFFSET    tt_sival_filter_end_offset()
#define INBOUND_BASE         tt_sival_inbound_filter_base()
#define SMC_INBOUND          tt_sival_smc_inbound()

#define FILTER_CONFIG_READ_EN     BIT64(0)
#define FILTER_CONFIG_WRITE_EN    BIT64(1)
#define FILTER_CONFIG_ADDR_MODE   BIT64(4)
#define FILTER_CONFIG_ALLOW_NS    BIT64(8)
#define FILTER_CONFIG_ALLOW_BURST BIT64(24)

#define FILTER_CONFIG_READ_EN BIT64(0)

static uint64_t read_entry_reg(uint32_t entry, size_t offset)
{
	return read64_reg(INBOUND_BASE + (entry * FILTER_STRIDE) + offset);
}

static void fill_entry(uint32_t entry, uint64_t val)
{
	write64_reg(INBOUND_BASE + (entry * FILTER_STRIDE) + FILTER_CONFIG_OFFSET, val);
	write64_reg(INBOUND_BASE + (entry * FILTER_STRIDE) + FILTER_START_OFFSET, val);
	write64_reg(INBOUND_BASE + (entry * FILTER_STRIDE) + FILTER_END_OFFSET, val);
}

static void before(void *f)
{
	ARG_UNUSED(f);
	tt_grendel_reg_stub_reset();
	fill_entry(0, 0xaaaaaaaaaaaaaaaaULL);
	fill_entry(1, 0xaaaaaaaaaaaaaaaaULL);
}

ZTEST(tt_grendel_filter, test_enable_programs_entry)
{
	struct tt_grendel_filter_flags flags = {
		.read = 1,
		.write = 1,
		.nonsecure = 1,
		.burst = 1,
	};
	int ret = tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0x08018000, 0x08037fff, flags);

	zassert_equal(ret, 0);
	zassert_equal(read_entry_reg(0, FILTER_START_OFFSET), 0x08018000);
	zassert_equal(read_entry_reg(0, FILTER_END_OFFSET), 0x08037fff);
	zassert_equal(read_entry_reg(0, FILTER_CONFIG_OFFSET),
		      FILTER_CONFIG_READ_EN | FILTER_CONFIG_WRITE_EN | FILTER_CONFIG_ADDR_MODE |
			      FILTER_CONFIG_ALLOW_NS | FILTER_CONFIG_ALLOW_BURST);
}

ZTEST(tt_grendel_filter, test_enable_read_only)
{
	struct tt_grendel_filter_flags flags = {
		.read = 1,
	};
	int ret = tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0x1000, 0x1fff, flags);

	zassert_equal(ret, 0);
	zassert_equal(read_entry_reg(0, FILTER_CONFIG_OFFSET),
		      FILTER_CONFIG_READ_EN | FILTER_CONFIG_ADDR_MODE);
}

ZTEST(tt_grendel_filter, test_enable_equal_range)
{
	struct tt_grendel_filter_flags flags = {
		.write = 1,
	};
	int ret = tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0x2000, 0x2000, flags);

	zassert_equal(ret, 0);
	zassert_equal(read_entry_reg(0, FILTER_START_OFFSET), 0x2000);
	zassert_equal(read_entry_reg(0, FILTER_END_OFFSET), 0x2000);
}

ZTEST(tt_grendel_filter, test_enable_does_not_touch_other_entry)
{
	struct tt_grendel_filter_flags flags = {
		.read = 1,
		.write = 1,
	};
	int ret = tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0x0, 0xff, flags);

	zassert_equal(ret, 0);
	zassert_equal(read_entry_reg(1, FILTER_CONFIG_OFFSET), 0xaaaaaaaaaaaaaaaaULL);
	zassert_equal(read_entry_reg(1, FILTER_START_OFFSET), 0xaaaaaaaaaaaaaaaaULL);
	zassert_equal(read_entry_reg(1, FILTER_END_OFFSET), 0xaaaaaaaaaaaaaaaaULL);
}

ZTEST(tt_grendel_filter, test_disable_clears_entry)
{
	struct tt_grendel_filter_flags flags = {
		.read = 1,
		.write = 1,
		.burst = 1,
	};
	int ret;

	ret = tt_grendel_filter_enable(SMC_INBOUND, 1, 0, 0x10, 0x1f, flags);
	zassert_equal(ret, 0);

	ret = tt_grendel_filter_disable(SMC_INBOUND, 1, 0);
	zassert_equal(ret, 0);
	zassert_equal(read_entry_reg(1, FILTER_CONFIG_OFFSET), 0);
	zassert_equal(read_entry_reg(1, FILTER_START_OFFSET), 0);
	zassert_equal(read_entry_reg(1, FILTER_END_OFFSET), 0);
}

ZTEST(tt_grendel_filter, test_enable_rejects_bad_args)
{
	struct tt_grendel_filter_flags rw = {
		.read = 1,
		.write = 1,
	};
	struct tt_grendel_filter_flags none = {0};
	uint64_t snap_cfg = read_entry_reg(0, FILTER_CONFIG_OFFSET);
	uint64_t snap_start = read_entry_reg(0, FILTER_START_OFFSET);
	uint64_t snap_end = read_entry_reg(0, FILTER_END_OFFSET);

	zassert_equal(tt_grendel_filter_enable(SMC_INBOUND, 16, 0, 0, 1, rw), -EINVAL);
	zassert_equal(tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0x20, 0x10, rw), -EINVAL);
	zassert_equal(tt_grendel_filter_enable(SMC_INBOUND, 0, 0, BIT64(56), BIT64(56), rw),
		      -EINVAL);
	zassert_equal(tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0, 1, none), -EINVAL);
	zassert_equal(read_entry_reg(0, FILTER_CONFIG_OFFSET), snap_cfg);
	zassert_equal(read_entry_reg(0, FILTER_START_OFFSET), snap_start);
	zassert_equal(read_entry_reg(0, FILTER_END_OFFSET), snap_end);
}

ZTEST(tt_grendel_filter, test_disable_rejects_bad_entry)
{
	uint64_t snap_cfg = read_entry_reg(1, FILTER_CONFIG_OFFSET);

	zassert_equal(tt_grendel_filter_disable(SMC_INBOUND, 16, 0), -EINVAL);
	zassert_equal(read_entry_reg(1, FILTER_CONFIG_OFFSET), snap_cfg);
}

ZTEST_SUITE(tt_grendel_filter, NULL, NULL, before, NULL, NULL);
