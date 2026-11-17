/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/fff.h>

#include <zephyr/drivers/i2c.h>

#include "gddr.h"
#include "noc2axi.h"
#include "reg_mock.h"
static const uint32_t mrisc_tlb = 13U;
static const uint32_t mrisc_msg_reg = ARC_NOC0_BASE_ADDR + (mrisc_tlb << NOC_TLB_LOG_SIZE) +
				      (MRISC_MSG_REGISTER & NOC_TLB_WINDOW_ADDR_MASK);

static uint32_t num_mrisc_msgs;
static uint32_t mrisc_msgs[NUM_GDDR];
uint32_t read_reg_fake_mrisc_busy(uint32_t addr)
{
	if (addr == mrisc_msg_reg) {
		return MRISC_MSG_TYPE_PHY_POWERDOWN;
	}

	return 0;
}

uint32_t read_reg_fake_mrisc_timed_out(uint32_t addr)
{
	if (addr == mrisc_msg_reg) {
		static uint32_t mrisc_msg_read_call_count;

		if (mrisc_msg_read_call_count < NUM_GDDR) {
			mrisc_msg_read_call_count++;
		} else {
			z_impl_sys_clock_tick_set(
				sys_clock_tick_get() +
				k_ms_to_ticks_ceil64(MRISC_POWER_SETTING_TIMEOUT_MS) + 1);
			return MRISC_MSG_TYPE_PHY_POWERDOWN;
		}
	}

	return 0;
}

void write_reg_fake_count_mrisc_msgs(uint32_t addr, uint32_t value)
{
	if (addr == mrisc_msg_reg) {
		if (num_mrisc_msgs < NUM_GDDR) {
			mrisc_msgs[num_mrisc_msgs] = value;
		}
		num_mrisc_msgs++;
	}
}

ZTEST(gddr, test_mrisc_busy_failed)
{
	ReadReg_fake.custom_fake = read_reg_fake_mrisc_busy;
	int32_t ret = set_mrisc_power_setting(true);

	zassert_equal(ret, -EBUSY);
}

ZTEST(gddr, test_mrisc_timed_out)
{
	ReadReg_fake.custom_fake = read_reg_fake_mrisc_timed_out;
	int32_t ret = set_mrisc_power_setting(true);

	zassert_equal(ret, -ETIMEDOUT);
}

ZTEST(gddr, test_mrisc_power_on)
{
	WriteReg_fake.custom_fake = write_reg_fake_count_mrisc_msgs;
	int32_t ret = set_mrisc_power_setting(true);

	zexpect_equal(ret, 0);
	zexpect_equal(num_mrisc_msgs, NUM_GDDR);

	for (uint8_t i = 0U; i < NUM_GDDR; i++) {
		zexpect_equal(mrisc_msgs[i], MRISC_MSG_TYPE_PHY_WAKEUP);
	}
	num_mrisc_msgs = 0U;
}

ZTEST(gddr, test_mrisc_power_off)
{
	WriteReg_fake.custom_fake = write_reg_fake_count_mrisc_msgs;
	int32_t ret = set_mrisc_power_setting(false);

	zexpect_equal(ret, 0);
	zexpect_equal(num_mrisc_msgs, NUM_GDDR);

	for (uint8_t i = 0U; i < NUM_GDDR; i++) {
		zexpect_equal(mrisc_msgs[i], MRISC_MSG_TYPE_PHY_POWERDOWN);
	}
	num_mrisc_msgs = 0U;
}

ZTEST_SUITE(gddr, NULL, NULL, NULL, NULL, NULL);
