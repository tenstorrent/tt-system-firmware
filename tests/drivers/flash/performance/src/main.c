/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/devicetree.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/irq.h>

#define TEST_AREA storage_partition

#define TEST_AREA_OFFSET PARTITION_OFFSET(TEST_AREA)
#define TEST_AREA_SIZE   PARTITION_SIZE(TEST_AREA)
#define TEST_AREA_MAX    (TEST_AREA_OFFSET + TEST_AREA_SIZE)
#define TEST_AREA_DEVICE PARTITION_DEVICE(TEST_AREA)
#define TEST_ITERATIONS  10

static const struct device *const flash_dev = TEST_AREA_DEVICE;
static uint8_t buf[TEST_AREA_SIZE];
static uint8_t check_buf[TEST_AREA_SIZE];

static int flash_program_wrap(const struct device *dev, off_t offset, const void *data, size_t len)
{
	int rc = flash_erase(dev, offset, len);

	if (rc != 0) {
		return rc;
	}
	return flash_write(dev, offset, data, len);
}

ZTEST(flash_driver_perf, test_read_perf)
{
	int rc;
	int64_t ts;
	int64_t delta = 0;

	for (int i = 0; i < TEST_ITERATIONS; i++) {
		ts = k_uptime_get();
		rc = flash_read(flash_dev, TEST_AREA_OFFSET, buf, TEST_AREA_SIZE);
		delta += k_uptime_delta(&ts);
		zassert_equal(rc, 0, "Cannot read flash");
	}
	delta /= TEST_ITERATIONS;
	TC_PRINT("Read performance test averaged %lld ms\n", delta);
	zassert_true(delta < CONFIG_EXPECTED_READ_TIME, "Read performance test failed");
}

ZTEST(flash_driver_perf, test_program_perf)
{
	int rc;
	int64_t ts;
	int64_t delta = 0;

	/* Create data to write to erased region */
	for (int i = 0; i < TEST_AREA_SIZE; i++) {
		buf[i] = (uint8_t)(i & 0xff);
	}

	for (int i = 0; i < TEST_ITERATIONS; i++) {
		/* Write buffer to flash */
		ts = k_uptime_get();
		rc = flash_program_wrap(flash_dev, TEST_AREA_OFFSET, buf, TEST_AREA_SIZE);
		delta += k_uptime_delta(&ts);
		zassert_equal(rc, 0, "Cannot program flash");
		/* Read back the data */
		rc = flash_read(flash_dev, TEST_AREA_OFFSET, check_buf, TEST_AREA_SIZE);
		zassert_equal(rc, 0, "Cannot read flash");
		/* Check that the data read back is the same as the data written */
		zassert_mem_equal(buf, check_buf, TEST_AREA_SIZE,
				  "Data read back from flash does not match data written");
		TC_PRINT("Iteration %i: data read back from flash matches data written\n", i);
	}
	delta /= TEST_ITERATIONS;

	TC_PRINT("Program performance test averaged %lld ms\n", delta);
	zassert_true(delta < CONFIG_EXPECTED_PROGRAM_TIME, "Program performance test failed");
}

ZTEST_SUITE(flash_driver_perf, NULL, NULL, NULL, NULL, NULL);
