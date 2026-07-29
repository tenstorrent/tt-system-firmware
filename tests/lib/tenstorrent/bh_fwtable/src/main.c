/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/misc/bh_fwtable.h>

#define FWTABLE_DEV DEVICE_DT_GET(DT_NODELABEL(fwtable))

/*
 * Per-board expected values used as the assertion source of truth.
 * Numbers come from the matching board's text protos under
 *   boards/tenstorrent/tt_blackhole/spirom_data_tables/<BOARD>/{read_only,fw_table}.txt
 *
 * To add support for another board, define a new struct with all fields
 * populated and add a case in expected_for_board_type().
 */
struct board_expected {
	const char *name;
	uint8_t board_type; /* the top byte (bits 36-43) of board_id */
	uint32_t vendor_id;
	uint32_t asic_fmax;
	uint32_t asic_fmin;
	uint32_t vdd_max;
	uint32_t vdd_min;
	uint32_t tdp_limit;
};

static const struct board_expected exp_p150a = {
	.name = "P150A",
	.board_type = 0x40,
	.vendor_id = 0x1e52,
	.asic_fmax = 1350,
	.asic_fmin = 800,
	.vdd_max = 900,
	.vdd_min = 700,
	.tdp_limit = 150,
};

static const struct board_expected exp_p150 = {
	.name = "P150",
	.board_type = 0x41,
	.vendor_id = 0x1e52,
	.asic_fmax = 1350,
	.asic_fmin = 800,
	.vdd_max = 900,
	.vdd_min = 700,
	.tdp_limit = 150,
};

static const struct board_expected exp_p150c = {
	.name = "P150C",
	.board_type = 0x42,
	.vendor_id = 0x1e52,
	.asic_fmax = 1350,
	.asic_fmin = 800,
	.vdd_max = 900,
	.vdd_min = 700,
	.tdp_limit = 500,
};

static const struct board_expected exp_p100a = {
	.name = "P100A",
	.board_type = 0x43,
	.vendor_id = 0x1e52,
	.asic_fmax = 1350,
	.asic_fmin = 800,
	.vdd_max = 900,
	.vdd_min = 700,
	.tdp_limit = 150,
};

static const struct board_expected exp_p300a = {
	.name = "P300A",
	.board_type = 0x45,
	.vendor_id = 0x1e52,
	.asic_fmax = 1350,
	.asic_fmin = 800,
	.vdd_max = 900,
	.vdd_min = 700,
	.tdp_limit = 86,
};

static const struct board_expected exp_p300b = {
	.name = "P300B",
	.board_type = 0x44,
	.vendor_id = 0x1e52,
	.asic_fmax = 1350,
	.asic_fmin = 800,
	.vdd_max = 900,
	.vdd_min = 700,
	.tdp_limit = 94,
};

static const struct board_expected exp_p300c = {
	.name = "P300C",
	.board_type = 0x46,
	.vendor_id = 0x1e52,
	.asic_fmax = 1350,
	.asic_fmin = 800,
	.vdd_max = 900,
	.vdd_min = 700,
	.tdp_limit = 125,
};

/* Lookup table — order doesn't matter, the resolver scans linearly. */
static const struct board_expected *const known_boards[] = {
	&exp_p150a, &exp_p150, &exp_p150c, &exp_p100a, &exp_p300a, &exp_p300b, &exp_p300c,
};

/* Resolve the per-board expected-values row from the board_type byte
 * extracted from the loaded read_only table. Returns NULL for boards we
 * haven't characterized yet.
 */
static const struct board_expected *expected_for_board_type(uint8_t board_type)
{
	for (size_t i = 0; i < ARRAY_SIZE(known_boards); i++) {
		if (known_boards[i]->board_type == board_type) {
			return known_boards[i];
		}
	}
	return NULL;
}

/* Populated in suite_setup, read by every other test. */
static const struct board_expected *expected;

/* Runs once before any test. The read_only table is read here; if the device
 * failed to load, test_0_device_ready (below) flags it as a hard failure.
 */
static void *suite_setup(void)
{
	const struct device *dev = FWTABLE_DEV;
	const struct _ReadOnly *ro = tt_bh_fwtable_get_read_only_table(dev);
	uint8_t board_type = (uint8_t)((ro->board_id >> 36) & 0xFF);

	expected = expected_for_board_type(board_type);
	return (void *)expected;
}

/* Runs FIRST in this suite. ztest orders tests by SORT_BY_NAME on the symbol
 * z_ztest_unit_test__<suite>__<fn>, so the "0_" prefix sorts this ahead of the
 * content tests below. The build guarantees flash.bin exists (CMake FATAL_ERRORs
 * when protoc is missing), so a not-ready device here is a real bh_fwtable
 * init/load regression and must FAIL — never skip.
 */
ZTEST(bh_fwtable_sim, test_0_device_ready)
{
	zassert_true(device_is_ready(FWTABLE_DEV),
		     "fwtable device not ready — bh_fwtable init/load (tt_bh_fwtable_load) "
		     "failed; check the driver's -EIO/-ENOMEM/-EINVAL load paths.");
}

ZTEST(bh_fwtable_sim, test_read_only_loaded)
{
	zassert_not_null(expected, "no expected-values table for this board; "
				   "add a case in expected_for_board_type()");

	const struct _ReadOnly *ro = tt_bh_fwtable_get_read_only_table(FWTABLE_DEV);

	zassert_not_null(ro, "read_only table pointer is NULL");
	zassert_equal(ro->vendor_id, expected->vendor_id, "[%s] vendor_id: expected 0x%x, got 0x%x",
		      expected->name, expected->vendor_id, ro->vendor_id);
	zassert_not_equal(ro->board_id, 0, "[%s] board_id is zero — did boardcfg actually load?",
			  expected->name);

	TC_PRINT("[%s] board_id  = 0x%llx\n", expected->name, (unsigned long long)ro->board_id);
	TC_PRINT("[%s] vendor_id = 0x%x\n", expected->name, ro->vendor_id);
}

ZTEST(bh_fwtable_sim, test_fw_table_loaded)
{
	zassert_not_null(expected, "no expected-values table for this board; "
				   "add a case in expected_for_board_type()");

	const struct _FwTable *fw = tt_bh_fwtable_get_fw_table(FWTABLE_DEV);

	zassert_not_null(fw, "fw_table pointer is NULL");
	zassert_equal(fw->chip_limits.asic_fmax, expected->asic_fmax,
		      "[%s] asic_fmax: expected %u, got %u", expected->name, expected->asic_fmax,
		      fw->chip_limits.asic_fmax);
	zassert_equal(fw->chip_limits.asic_fmin, expected->asic_fmin,
		      "[%s] asic_fmin: expected %u, got %u", expected->name, expected->asic_fmin,
		      fw->chip_limits.asic_fmin);
	zassert_equal(fw->chip_limits.vdd_max, expected->vdd_max,
		      "[%s] vdd_max: expected %u, got %u", expected->name, expected->vdd_max,
		      fw->chip_limits.vdd_max);
	zassert_equal(fw->chip_limits.vdd_min, expected->vdd_min,
		      "[%s] vdd_min: expected %u, got %u", expected->name, expected->vdd_min,
		      fw->chip_limits.vdd_min);
	zassert_equal(fw->chip_limits.tdp_limit, expected->tdp_limit,
		      "[%s] tdp_limit: expected %u, got %u", expected->name, expected->tdp_limit,
		      fw->chip_limits.tdp_limit);

	TC_PRINT("[%s] asic_fmax = %u, asic_fmin = %u\n", expected->name, fw->chip_limits.asic_fmax,
		 fw->chip_limits.asic_fmin);
	TC_PRINT("[%s] fw_bundle_version = 0x%x\n", expected->name, fw->fw_bundle_version);
}

ZTEST(bh_fwtable_sim, test_flash_info_loaded)
{
	const struct _FlashInfoTable *fi = tt_bh_fwtable_get_flash_info_table(FWTABLE_DEV);

	zassert_not_null(fi, "flash_info pointer is NULL");
	TC_PRINT("reprogrammed_count = %u\n", fi->reprogrammed_count);
	TC_PRINT("tt_flash_version   = 0x%x\n", fi->tt_flash_version);
}

ZTEST_SUITE(bh_fwtable_sim, NULL, suite_setup, NULL, NULL, NULL);
