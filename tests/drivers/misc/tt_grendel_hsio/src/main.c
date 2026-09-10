/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/drivers/misc/tt_grendel_hsio.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "regs.h"
#include "sival_addrs.h"

void tt_grendel_reg_stub_reset(void);

#define FILTER_CONFIG_READ_EN     BIT64(0)
#define FILTER_CONFIG_WRITE_EN    BIT64(1)
#define FILTER_CONFIG_ADDR_MODE   BIT64(4)
#define FILTER_CONFIG_ALLOW_NS    BIT64(8)
#define FILTER_CONFIG_ALLOW_BURST BIT64(24)

#define ALLOW_ALL_NS                                                                               \
	(FILTER_CONFIG_READ_EN | FILTER_CONFIG_WRITE_EN | FILTER_CONFIG_ADDR_MODE |                \
	 FILTER_CONFIG_ALLOW_NS | FILTER_CONFIG_ALLOW_BURST)
#define ALLOW_ALL_S                                                                                \
	(FILTER_CONFIG_READ_EN | FILTER_CONFIG_WRITE_EN | FILTER_CONFIG_ADDR_MODE |                \
	 FILTER_CONFIG_ALLOW_BURST)

static void before(void *f)
{
	ARG_UNUSED(f);
	tt_grendel_reg_stub_reset();
}

static void assert_allow_all_bank(uint64_t base)
{
	uint32_t start_off = tt_sival_filter_start_offset();
	uint32_t end_off = tt_sival_filter_end_offset();
	uint32_t stride = tt_sival_filter_stride();
	uint64_t mask = tt_sival_firewall_addr_mask();

	zassert_equal(read64_reg(base + start_off), 0);
	zassert_equal(read64_reg(base + end_off), mask);
	zassert_equal(read64_reg(base), ALLOW_ALL_NS);
	zassert_equal(read64_reg(base + stride + start_off), 0);
	zassert_equal(read64_reg(base + stride + end_off), mask);
	zassert_equal(read64_reg(base + stride), ALLOW_ALL_S);
}

ZTEST(tt_grendel_hsio, test_init_programs_reset_clock_and_filters)
{
	int ret = tt_grendel_hsio_init(0);

	zassert_equal(ret, 0);
	zassert_true((read32_reg(tt_sival_smc_cold_reset_n_addr()) & tt_sival_ker_rst_hsio0()) !=
		     0);
	zassert_equal(read32_reg(tt_sival_hsio0_cold_reset_n_addr()), 0x1FFFF);
	zassert_equal(read32_reg(tt_sival_hsio0_ag_mux_addr()) & GENMASK(11, 0), 0xFFF);
	assert_allow_all_bank(tt_sival_smn_hsio2hsio_filter_base());
	assert_allow_all_bank(tt_sival_smn_hsio2smn_filter_base());
	assert_allow_all_bank(tt_sival_hsio_noc2axi_filter_base());
	assert_allow_all_bank(tt_sival_hsio_pcie_filter_base());
}

ZTEST(tt_grendel_hsio, test_init_rejects_bad_tile)
{
	zassert_equal(tt_grendel_hsio_init(5), -EINVAL);
	zassert_equal(read32_reg(tt_sival_smc_cold_reset_n_addr()), 0);
}

ZTEST_SUITE(tt_grendel_hsio, NULL, NULL, before, NULL, NULL);
