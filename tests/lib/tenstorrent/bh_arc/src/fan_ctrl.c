/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>

#include <zephyr/ztest.h>

extern uint32_t fan_curve(float max_asic_temp, float max_gddr_temp);

ZTEST(fan_ctrl, test_fan_curve_asic_temp)
{
	/* Sweep the ASIC temperature while holding GDDR temp low */
	zassert_equal(fan_curve(25, 25), 20);
	zassert_equal(fan_curve(30, 25), 20);
	zassert_equal(fan_curve(35, 25), 20);
	zassert_equal(fan_curve(40, 25), 20);
	zassert_equal(fan_curve(45, 25), 20);
	zassert_equal(fan_curve(50, 25), 20);
	zassert_equal(fan_curve(55, 25), 20);
	zassert_equal(fan_curve(60, 25), 20);
	zassert_equal(fan_curve(65, 25), 24);
	zassert_equal(fan_curve(70, 25), 28);
	zassert_equal(fan_curve(75, 25), 40);
	zassert_equal(fan_curve(80, 25), 60);
	zassert_equal(fan_curve(85, 25), 80);
	zassert_equal(fan_curve(90, 25), 100);
	zassert_equal(fan_curve(95, 25), 100);
}

ZTEST(fan_ctrl, test_fan_curve_gddr_temp)
{
	/* Sweep the GDDR temperature upward while holding ASIC temp low */
	/* A monotonically rising GDDR temp is tracked immediately by the hysteresis logic */
	zassert_equal(fan_curve(25, 25), 20);
	zassert_equal(fan_curve(25, 30), 20);
	zassert_equal(fan_curve(25, 35), 20);
	zassert_equal(fan_curve(25, 40), 20);
	zassert_equal(fan_curve(25, 45), 20);
	zassert_equal(fan_curve(25, 50), 25);
	zassert_equal(fan_curve(25, 55), 30);
	zassert_equal(fan_curve(25, 60), 34);
	zassert_equal(fan_curve(25, 65), 49);
	zassert_equal(fan_curve(25, 70), 64);
	zassert_equal(fan_curve(25, 75), 79);
	zassert_equal(fan_curve(25, 80), 94);
	zassert_equal(fan_curve(25, 85), 100);
	zassert_equal(fan_curve(25, 90), 100);
	zassert_equal(fan_curve(25, 95), 100);
}

ZTEST(fan_ctrl, test_fan_curve_gddr_hysteresis)
{
	/* Prime the effective temp to a known peak */
	(void)fan_curve(25, -50);
	zassert_equal(fan_curve(25, 90), 100);

	/* Drop to 70 (>= 5 below the prior peak) -> effective tracks down to 70 */
	zassert_equal(fan_curve(25, 70), 64);
	/* Small drops (< 5 from 70): fan speed stays put */
	zassert_equal(fan_curve(25, 67), 64);
	zassert_equal(fan_curve(25, 66), 64);
	/* A cumulative 5-degree drop (to 65) finally lowers the effective temp */
	zassert_equal(fan_curve(25, 65), 49);
	/* A rise is followed immediately */
	zassert_equal(fan_curve(25, 80), 94);
}

ZTEST(fan_ctrl, test_fan_curve_bounds)
{
	/* Test boundary conditions */
	static const float temps[] = {
		-INFINITY, /* negative-most condition */
		-35,       /* darn cold */
		-1,        /* on the boundary */
		0,         /* inflection point */
		1,         /* on the boundary */
		23,        /* ~room temp */
		50,        /* pretty warm */
		100,       /* hot! */
		300,       /* on fire */
		INFINITY,  /* positive-most condition */
	};

	for (size_t i = 0; i < ARRAY_SIZE(temps); ++i) {
		for (size_t j = 0; j < ARRAY_SIZE(temps); ++j) {
			uint32_t pct = fan_curve(temps[i], temps[j]);

			zassert_true(pct >= 0, "unexpected pct %u for fan_curve(%f, %f)", pct,
				     (double)temps[i], (double)temps[j]);
			zassert_true(pct <= 100, "unexpected pct %u for fan_curve(%f, %f)", pct,
				     (double)temps[i], (double)temps[j]);
		}
	}
}

ZTEST_SUITE(fan_ctrl, NULL, NULL, NULL, NULL, NULL);
