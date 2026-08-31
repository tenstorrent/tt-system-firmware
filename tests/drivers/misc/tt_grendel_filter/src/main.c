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

#include <firewall.h>
#include <platform.h>

#include "regs.h"

DEFINE_FFF_GLOBALS;

DEFINE_FAKE_VOID_FUNC(write16_reg, uint64_t, uint16_t);
DEFINE_FAKE_VALUE_FUNC(uint16_t, read16_reg, uint64_t);
DEFINE_FAKE_VOID_FUNC(write32_reg, uint64_t, uint32_t);
DEFINE_FAKE_VALUE_FUNC(uint32_t, read32_reg, uint64_t);
DEFINE_FAKE_VOID_FUNC(write64_reg, uint64_t, uint64_t);
DEFINE_FAKE_VALUE_FUNC(uint64_t, read64_reg, uint64_t);

#define FILTER_STRIDE        FIREWALL_FILTER_SIZE
#define FILTER_CONFIG_OFFSET FIREWALL_FILTER_CONFIG_OFFSET
#define FILTER_START_OFFSET  FIREWALL_START_ADDR_OFFSET
#define FILTER_END_OFFSET    FIREWALL_END_ADDR_OFFSET
#define INBOUND_BASE         SMC_CPU_SMC_INBOUND_FILTER_CTRL_0_REG_MAP_BASE_ADDR

#define FILTER_CONFIG_READ_EN     BIT64(0)
#define FILTER_CONFIG_WRITE_EN    BIT64(1)
#define FILTER_CONFIG_ADDR_MODE   BIT64(4)
#define FILTER_CONFIG_ALLOW_NS    BIT64(8)
#define FILTER_CONFIG_ALLOW_BURST BIT64(24)

static uint64_t entry_reg_addr(uint32_t entry, size_t offset)
{
	return INBOUND_BASE + (entry * FILTER_STRIDE) + offset;
}

static void assert_write64(uint64_t addr, uint64_t value)
{
	for (unsigned int i = write64_reg_fake.call_count; i > 0; i--) {
		unsigned int index = i - 1;

		if (write64_reg_fake.arg0_history[index] == addr) {
			zassert_equal(write64_reg_fake.arg1_history[index], value);
			return;
		}
	}

	zassert_true(false, "Expected write to 0x%llx", addr);
}

static void before(void *f)
{
	ARG_UNUSED(f);
	RESET_FAKE(write16_reg);
	RESET_FAKE(read16_reg);
	RESET_FAKE(write32_reg);
	RESET_FAKE(read32_reg);
	RESET_FAKE(write64_reg);
	RESET_FAKE(read64_reg);
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
	zassert_equal(write64_reg_fake.call_count, 3);
	assert_write64(entry_reg_addr(0, FILTER_START_OFFSET), 0x08018000);
	assert_write64(entry_reg_addr(0, FILTER_END_OFFSET), 0x08037fff);
	assert_write64(entry_reg_addr(0, FILTER_CONFIG_OFFSET),
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
	zassert_equal(write64_reg_fake.call_count, 3);
	assert_write64(entry_reg_addr(0, FILTER_CONFIG_OFFSET),
		       FILTER_CONFIG_READ_EN | FILTER_CONFIG_ADDR_MODE);
}

ZTEST(tt_grendel_filter, test_enable_equal_range)
{
	struct tt_grendel_filter_flags flags = {
		.write = 1,
	};
	int ret = tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0x2000, 0x2000, flags);

	zassert_equal(ret, 0);
	zassert_equal(write64_reg_fake.call_count, 3);
	assert_write64(entry_reg_addr(0, FILTER_START_OFFSET), 0x2000);
	assert_write64(entry_reg_addr(0, FILTER_END_OFFSET), 0x2000);
}

ZTEST(tt_grendel_filter, test_enable_does_not_touch_other_entry)
{
	struct tt_grendel_filter_flags flags = {
		.read = 1,
		.write = 1,
	};
	int ret = tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0x0, 0xff, flags);

	zassert_equal(ret, 0);
	zassert_equal(write64_reg_fake.call_count, 3);
	for (unsigned int i = 0; i < write64_reg_fake.call_count; i++) {
		zassert_true(write64_reg_fake.arg0_history[i] >= INBOUND_BASE &&
			     write64_reg_fake.arg0_history[i] < INBOUND_BASE + FILTER_STRIDE);
	}
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
	zassert_equal(write64_reg_fake.call_count, 6);
	assert_write64(entry_reg_addr(1, FILTER_CONFIG_OFFSET), 0);
	assert_write64(entry_reg_addr(1, FILTER_START_OFFSET), 0);
	assert_write64(entry_reg_addr(1, FILTER_END_OFFSET), 0);
}

ZTEST(tt_grendel_filter, test_enable_rejects_bad_args)
{
	struct tt_grendel_filter_flags rw = {
		.read = 1,
		.write = 1,
	};
	struct tt_grendel_filter_flags none = {0};

	zassert_equal(tt_grendel_filter_enable(SMC_INBOUND, 16, 0, 0, 1, rw), -EINVAL);
	zassert_equal(tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0x20, 0x10, rw), -EINVAL);
	zassert_equal(tt_grendel_filter_enable(SMC_INBOUND, 0, 0, BIT64(56), BIT64(56), rw),
		      -EINVAL);
	zassert_equal(tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0, 1, none), -EINVAL);
	zassert_equal(write64_reg_fake.call_count, 0);
}

ZTEST(tt_grendel_filter, test_disable_rejects_bad_entry)
{
	zassert_equal(tt_grendel_filter_disable(SMC_INBOUND, 16, 0), -EINVAL);
	zassert_equal(write64_reg_fake.call_count, 0);
}

ZTEST_SUITE(tt_grendel_filter, NULL, NULL, before, NULL, NULL);
