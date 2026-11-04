/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/clock_control_tt_bh.h>
#include <zephyr/sys/printk.h>

ZTEST(clock_control_rate, test_get_rate_aiclk)
{
	const struct device *pll = DEVICE_DT_GET(DT_NODELABEL(pll0));
	uint32_t clock_rate;
	int ret;

	/* Use the actual enum for type safety and clarity */
	clock_control_subsys_t aiclk_subsys =
		(clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_AICLK;

	ret = clock_control_get_rate(pll, aiclk_subsys, &clock_rate);
	zassert_ok(ret, "clock_control_get_rate for AICLK failed with %d", ret);
	zassert_equal(clock_rate, 3200, "AICLK rate is %d", clock_rate);
}

ZTEST(clock_control_rate, test_set_rate_gddr)
{
	const struct device *pll = DEVICE_DT_GET(DT_NODELABEL(pll0));
	uint32_t new_rate;
	int ret;

	clock_control_subsys_t gddr_subsys =
		(clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_GDDRMEMCLK;

	for (uint32_t target_rate = CONFIG_CLOCK_CTRL_GDDRCLK_MIN_RATE;
	     target_rate <= CONFIG_CLOCK_CTRL_GDDRCLK_MAX_RATE;
	     target_rate += CONFIG_CLOCK_CTRL_GDRRCLK_FREQ_INCR) {
		ret = clock_control_set_rate(pll, gddr_subsys,
					     (clock_control_subsys_rate_t)target_rate);
		zassert_ok(ret, "set_rate for GDDR failed with %d", ret);

		ret = clock_control_get_rate(pll, gddr_subsys, &new_rate);
		zassert_ok(ret, "get_rate for GDDR failed with %d", ret);

		zassert_within(new_rate, target_rate,
			       new_rate / 100.f * CONFIG_CLOCK_CTRL_TOLERANCE_PERCENT,
			       "Expected ~%d Hz but got %d Hz", target_rate, new_rate);
	}
}

ZTEST(clock_control_rate, test_set_rate_aiclk)
{
	const struct device *pll = DEVICE_DT_GET(DT_NODELABEL(pll0));
	uint32_t new_rate;
	int ret;

	clock_control_subsys_t aiclk_subsys =
		(clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_AICLK;

	for (uint32_t target_rate = CONFIG_CLOCK_CTRL_AICLK_MIN_RATE;
	     target_rate <= CONFIG_CLOCK_CTRL_AICLK_MAX_RATE;
	     target_rate += CONFIG_CLOCK_CTRL_AICLK_FREQ_INCR) {
		ret = clock_control_set_rate(pll, aiclk_subsys,
					     (clock_control_subsys_rate_t)target_rate);
		zassert_ok(ret, "set_rate for AICLK failed with %d", ret);

		ret = clock_control_get_rate(pll, aiclk_subsys, &new_rate);
		zassert_ok(ret, "get_rate for AICLK failed with %d", ret);

		zassert_within(new_rate, target_rate,
			       new_rate / 100.f * CONFIG_CLOCK_CTRL_TOLERANCE_PERCENT,
			       "Expected ~%d Hz but got %d Hz", target_rate, new_rate);
	}
}

ZTEST(clock_control_rate, test_set_rate_l2cpu)
{
	const struct device *pll = DEVICE_DT_GET(DT_NODELABEL(pll4));
	static const uint32_t targets[] = {200, 1750, 1000, 800};
	static const enum clock_control_tt_bh_clock outputs[] = {
		CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_0, CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_1,
		CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_2, CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_3};
	uint32_t rate;
	int ret;

	zassert_true(device_is_ready(pll), "pll4 not ready");

	ARRAY_FOR_EACH(targets, t) {
		ret = clock_control_set_rate(
			pll, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_0,
			(clock_control_subsys_rate_t)targets[t]);
		zassert_ok(ret, "set_rate(L2CPUCLK, %u) failed with %d", targets[t], ret);

		ARRAY_FOR_EACH(outputs, o) {
			ret = clock_control_get_rate(pll, (clock_control_subsys_t)outputs[o],
						     &rate);
			zassert_ok(ret, "get_rate(L2CPUCLK_%zu) failed with %d", o, ret);
			if (rate == 0) {
				/* Output gated off; set_rate leaves it alone */
				continue;
			}
			zassert_equal(rate, targets[t], "L2CPUCLK_%zu: expected %u MHz, got %u", o,
				      targets[t], rate);
		}
	}
}

ZTEST(clock_control_rate, test_set_rate_l2cpu_out_of_range_fails)
{
	const struct device *pll = DEVICE_DT_GET(DT_NODELABEL(pll4));
	const clock_control_subsys_t l2cpuclk =
		(clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_0;
	uint32_t before, after;
	int ret;

	zassert_ok(clock_control_get_rate(pll, l2cpuclk, &before));

	/* 3000 MHz needs a VCO above its maximum even at the smallest postdiv */
	ret = clock_control_set_rate(pll, l2cpuclk, (clock_control_subsys_rate_t)3000);
	zassert_equal(-ERANGE, ret, "Expected -ERANGE but got %d", ret);

	/* Rejected requests must not touch the dividers */
	zassert_ok(clock_control_get_rate(pll, l2cpuclk, &after));
	zassert_equal(before, after, "Rate changed from %u to %u on rejected request", before,
		      after);
}

ZTEST(clock_control_rate, test_set_rate_out_of_range_fails)
{
	const struct device *pll = DEVICE_DT_GET(DT_NODELABEL(pll0));
	const uint32_t target_rate = 1000000;
	int ret;

	clock_control_subsys_t gddr_subsys =
		(clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_GDDRMEMCLK;

	ret = clock_control_set_rate(pll, gddr_subsys, (clock_control_subsys_rate_t)target_rate);
	zassert_equal(-ERANGE, ret, "Expected -ERANGE but got %d", ret);
}

ZTEST_SUITE(clock_control_rate, NULL, NULL, NULL, NULL, NULL);

ZTEST(clock_control_config, test_configure_unsupported_fails)
{
	const struct device *pll = DEVICE_DT_GET(DT_NODELABEL(pll0));
	void *invalid_option = (void *)99;
	int ret;

	ret = clock_control_configure(pll, NULL, invalid_option);
	zassert_equal(-ENOTSUP, ret, "Expected -ENOTSUP but got %d", ret);
}

ZTEST(clock_control_config, test_configure_bypass_succeeds)
{
	const struct device *pll = DEVICE_DT_GET(DT_NODELABEL(pll0));
	uint32_t initial_rate;
	int ret;

	clock_control_subsys_t aiclk_subsys =
		(clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_AICLK;

	ret = clock_control_get_rate(pll, aiclk_subsys, &initial_rate);
	zassert_ok(ret, "Failed to get initial AICLK rate");

	void *bypass_option = (void *)CLOCK_CONTROL_TT_BH_CONFIG_BYPASS;

	ret = clock_control_configure(pll, NULL, bypass_option);
	zassert_ok(ret, "clock_control_configure failed with %d", ret);
}

ZTEST_SUITE(clock_control_config, NULL, NULL, NULL, NULL, NULL);
