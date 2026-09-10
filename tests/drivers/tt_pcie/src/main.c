/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/ztest.h>

#include <soc.h>

#define PCIE0_NODE DT_NODELABEL(pcie0)

static const struct device *const pcie0 = DEVICE_DT_GET(PCIE0_NODE);

/*
 * The only thing visible from the host while the tests run are the scratch
 * registers which the harness prints on every poll. Each step publishes which
 * test it is in and how far it got, so a run that stops responding names the
 * call it stopped in instead of just failing to reach the pass token.
 * scratch[4] carries the value the step just produced.
 *
 * The tag makes step 0 distinguishable from a scratch register nobody has
 * written yet.
 */
#define STEP(test, step) WRITE_SCRATCH(3, 0xB0000000U | ((test) << 8) | (step))
#define STEP_RESULT(val) WRITE_SCRATCH(4, (uint32_t)(val))

#define T_READY 0x00U

/*
 * A failing assertion ends its test but not the suite, and the suite's pass
 * token is only written if every test passed -- so without this a run with one
 * broken test is indistinguishable from a run with six. Each test sets its bit
 * on the way out; whatever is missing at the end failed.
 */
static uint32_t passed_mask;

#define PASSED(bit)                                                                                \
	do {                                                                                       \
		passed_mask |= BIT(bit);                                                           \
		WRITE_SCRATCH(1, 0xA5000000U | passed_mask);                                       \
	} while (0)

#define P_READY 0

ZTEST(tt_pcie, test_endpoint_ready)
{
	STEP(T_READY, 1);
	STEP_RESULT(device_is_ready(pcie0));
	zassert_true(device_is_ready(pcie0), "pcie0 not ready");
	STEP(T_READY, 2);
	PASSED(P_READY);

	/*
	 * Device init trained the link (HW_INIT). Stay in a busy loop so the
	 * endpoint remains enumerated; do not sleep (emul mtime is ~10x slow).
	 * The suite never reaches TC_END_REPORT, so write the pass token here.
	 */
	test_pass();
	while (1) {
	}
}

/*
 * The harness's verdict is ztest's own, and ztest can fail a run for reasons
 * no individual test reports. With no readable console, publishing the
 * framework's counters separates "a test failed" from "every test passed and
 * the run was failed anyway". Reached only if test_endpoint_ready returned.
 */
static void tt_pcie_teardown(void *fixture)
{
	uint32_t run = 0;
	uint32_t pass = 0;
	uint32_t fail = 0;
	uint32_t skip = 0;
	uint32_t inconsistent = 0;

	ARG_UNUSED(fixture);

	for (struct ztest_unit_test *t = _ztest_unit_test_list_start; t < _ztest_unit_test_list_end;
	     t++) {
		run += t->stats->run_count;
		pass += t->stats->pass_count;
		fail += t->stats->fail_count;
		skip += t->stats->skip_count;

		if (t->stats->pass_count + t->stats->fail_count + t->stats->skip_count !=
		    t->stats->run_count) {
			inconsistent = 1;
		}
	}

	WRITE_SCRATCH(4, 0xC0000000U | (run << 16) | (pass << 12) | (fail << 8) | (skip << 4) |
				 inconsistent);
}

ZTEST_SUITE(tt_pcie, NULL, NULL, NULL, NULL, tt_pcie_teardown);
