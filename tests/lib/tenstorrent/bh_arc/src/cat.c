/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/ztest.h>
#include <zephyr/sys/util.h>

#include "cat.h"

/* Representative thresholds for exercising the state machine; these are local
 * to the test and independent of any firmware-table values.
 */
#define HOT      95              /* at the sustained threshold */
#define COLD     (HOT - 1)       /* just below the threshold */
#define CRITICAL 110             /* immediate-trip threshold */
#define DUR      (1 * 60 * 1000) /* sustained dwell time, ms */

/* Exercise the monitor with the default thresholds. */
#define MONITOR(now, temp) EvaluateGddrThermTrip((now), (temp), HOT, CRITICAL, DUR)

/* Run one cold pass to clear the monitor's internal tracking between tests */
static void cool(void)
{
	(void)MONITOR(0, COLD);
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	cool();
}

ZTEST(cat, test_all_cold_never_trips)
{
	/* Not hot, so even after a long time nothing trips */
	zassert_equal(MONITOR(DUR + 1000, COLD), 0);
}

ZTEST(cat, test_brief_spike_does_not_trip)
{
	zassert_equal(MONITOR(0, HOT), 0, "should not trip on first hot reading");

	/* Hot, but not yet for the full duration */
	zassert_equal(MONITOR(DUR - 1, HOT), 0, "should not trip prior to duration elapsing");

	/* Cools off before the window completes, must not trip afterward */
	zassert_equal(MONITOR(DUR + 1000, COLD), 0, "cooling off must reset the timer");
}

ZTEST(cat, test_sustained_over_temp_trips)
{
	zassert_equal(MONITOR(0, HOT), 0);

	/* One millisecond short of the duration, still no trip */
	zassert_equal(MONITOR(DUR - 1, HOT), 0);

	/* Exactly at the duration boundary (>=), trip returning the max temp */
	zassert_equal(MONITOR(DUR, HOT), HOT, "sustained over-temp must trip");
}

ZTEST(cat, test_critical_temp_trips_immediately)
{
	/* At or above the critical temperature there is no dwell requirement */
	zassert_equal(MONITOR(0, CRITICAL), CRITICAL, "critical temp must trip immediately");
}

ZTEST(cat, test_zero_thresholds_never_trip)
{
	/* Unset (0) thresholds disable their trip path, even at a scorching temp. */
	zassert_equal(EvaluateGddrThermTrip(0, 200, 0, 0, DUR), 0,
		      "zero thresholds must not trip on the first reading");
	zassert_equal(EvaluateGddrThermTrip(DUR + 1000, 200, 0, 0, DUR), 0,
		      "zero thresholds must not trip after the dwell time either");
}

ZTEST_SUITE(cat, NULL, NULL, before, NULL, NULL);
