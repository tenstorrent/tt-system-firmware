/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/ztest.h>
#include <zephyr/sys/util.h>

#include "cat.h"

#define HOT      CAT_GDDR_THERM_TRIP_TEMP          /* at the sustained threshold */
#define COLD     (CAT_GDDR_THERM_TRIP_TEMP - 1)    /* just below the threshold */
#define CRITICAL CAT_GDDR_THERM_TRIP_CRITICAL_TEMP /* immediate-trip threshold */
#define DUR      CAT_GDDR_THERM_TRIP_DURATION_MS

/* Run one cold pass to clear the monitor's internal tracking between tests */
static void cool(void)
{
	(void)MonitorGddrThermTrip(0, COLD);
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	cool();
}

ZTEST(cat, test_all_cold_never_trips)
{
	/* Not hot, so even after a long time nothing trips */
	zassert_equal(MonitorGddrThermTrip(DUR + 1000, COLD), 0);
}

ZTEST(cat, test_brief_spike_does_not_trip)
{
	zassert_equal(MonitorGddrThermTrip(0, HOT), 0, "should not trip on first hot reading");

	/* Hot, but not yet for the full duration */
	zassert_equal(MonitorGddrThermTrip(DUR - 1, HOT), 0,
		      "should not trip before duration elapses");

	/* Cools off before the window completes, must not trip afterward */
	zassert_equal(MonitorGddrThermTrip(DUR + 1000, COLD), 0,
		      "cooling off must reset the timer");
}

ZTEST(cat, test_sustained_over_temp_trips)
{
	zassert_equal(MonitorGddrThermTrip(0, HOT), 0);

	/* One millisecond short of the duration, still no trip */
	zassert_equal(MonitorGddrThermTrip(DUR - 1, HOT), 0);

	/* Exactly at the duration boundary (>=), trip returning the max temp */
	zassert_equal(MonitorGddrThermTrip(DUR, HOT), HOT, "sustained over-temp must trip");
}

ZTEST(cat, test_critical_temp_trips_immediately)
{
	/* At or above the critical temperature there is no dwell requirement */
	zassert_equal(MonitorGddrThermTrip(0, CRITICAL), CRITICAL,
		      "critical temp must trip immediately");
}

ZTEST_SUITE(cat, NULL, NULL, before, NULL, NULL);
