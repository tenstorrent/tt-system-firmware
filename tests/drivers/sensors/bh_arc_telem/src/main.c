/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/smbus.h>
#include <zephyr/drivers/sensor/tenstorrent/pvt_tt_bh.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <telemetry.h>

#include <stdint.h>

static const struct device *const smc_sensor = DEVICE_DT_GET(DT_NODELABEL(smc_sensor));
static const struct device *const smc_smbus_bus =
	DEVICE_DT_GET(DT_BUS(DT_PHANDLE(DT_NODELABEL(smc_sensor), arc)));

SENSOR_DT_READ_IODEV(temp_iodev, DT_NODELABEL(smc_sensor), {SENSOR_CHAN_DIE_TEMP, 0});
SENSOR_DT_READ_IODEV(tdp_iodev, DT_NODELABEL(smc_sensor), {SENSOR_CHAN_POWER, 0});
SENSOR_DT_READ_IODEV(temp_tdp_iodev, DT_NODELABEL(smc_sensor), {SENSOR_CHAN_DIE_TEMP, 0},
		     {SENSOR_CHAN_POWER, 0});

RTIO_DEFINE(test_rtio, 2, 2);

#define TELEM_REFRESH_MS      100
#define TELEM_TOLERANCE_MICRO 300000 /* ±300mK tolerance for float conversions */

static void set_emulated_temp_and_wait(float temp_c)
{
	int rc = pvt_tt_bh_emul_set_ts_raw(pvt_tt_bh_temp_to_raw(temp_c));

	zassert_ok(rc, "failed to set emulated PVT temperature: %d", rc);
	k_msleep(TELEM_REFRESH_MS + 20);
}

static int read_channel(const struct rtio_iodev *iodev, enum sensor_channel channel_type,
			uint8_t channel_index, struct sensor_value *value)
{
	const struct sensor_decoder_api *decoder;
	struct sensor_chan_spec channel = {
		.chan_type = channel_type,
		.chan_idx = channel_index,
	};
	uint8_t read_buf[64];
	size_t base_size;
	size_t frame_size;
	int rc;

	rc = sensor_get_decoder(smc_sensor, &decoder);
	if (rc != 0) {
		return rc;
	}

	rc = decoder->get_size_info(channel, &base_size, &frame_size);
	if (rc != 0) {
		return rc;
	}

	if (base_size > sizeof(read_buf)) {
		return -ENOMEM;
	}

	rc = sensor_read(iodev, &test_rtio, read_buf, base_size);
	if (rc != 0) {
		return rc;
	}

	struct sensor_decode_context decode_ctx =
		SENSOR_DECODE_CONTEXT_INIT(decoder, read_buf, channel_type, channel_index);

	rc = sensor_decode(&decode_ctx, value, 1U);
	if (rc <= 0) {
		return (rc < 0) ? rc : -ENODATA;
	}

	return 0;
}

static void *bh_arc_telem_setup(void)
{
	uint32_t smbus_cfg = SMBUS_MODE_CONTROLLER | SMBUS_MODE_PEC;
	int rc;

	zassert_true(device_is_ready(smc_sensor));
	zassert_true(device_is_ready(smc_smbus_bus));

	rc = smbus_get_config(smc_smbus_bus, &smbus_cfg);
	if (rc == 0) {
		smbus_cfg |= SMBUS_MODE_PEC;
	} else {
		smbus_cfg = SMBUS_MODE_CONTROLLER | SMBUS_MODE_PEC;
	}

	rc = smbus_configure(smc_smbus_bus, smbus_cfg);
	zassert_ok(rc, "failed to enable SMBus PEC: %d", rc);

	set_emulated_temp_and_wait(25.5f);

	return NULL;
}

ZTEST(bh_arc_telem, test_temperature_read)
{
	struct sensor_value value;
	int rc;

	zassert_true(device_is_ready(smc_sensor));

	set_emulated_temp_and_wait(25.5f);

	rc = read_channel(&temp_iodev, SENSOR_CHAN_DIE_TEMP, 0U, &value);
	zassert_ok(rc, "temperature read failed: %d", rc);
	zassert_within(sensor_value_to_micro(&value), 25500000LL, TELEM_TOLERANCE_MICRO);
}

ZTEST(bh_arc_telem, test_temperature_negative_read)
{
	struct sensor_value value;
	int rc;

	set_emulated_temp_and_wait(-5.25f);

	rc = read_channel(&temp_iodev, SENSOR_CHAN_DIE_TEMP, 0U, &value);
	zassert_ok(rc, "negative temperature read failed: %d", rc);
	zassert_within(sensor_value_to_micro(&value), -5250000LL, TELEM_TOLERANCE_MICRO);
}

ZTEST(bh_arc_telem, test_temperature_reflects_latest_update)
{
	struct sensor_value value;
	int rc;
	const int iterations = 50;

	for (int i = 0; i < iterations; i++) {
		set_emulated_temp_and_wait(30.0f);

		rc = read_channel(&temp_iodev, SENSOR_CHAN_DIE_TEMP, 0U, &value);
		zassert_ok(rc, "iter %d first temperature read failed: %d", i, rc);
		zassert_within(sensor_value_to_micro(&value), 30000000LL, TELEM_TOLERANCE_MICRO);

		set_emulated_temp_and_wait(31.5f);

		rc = read_channel(&temp_iodev, SENSOR_CHAN_DIE_TEMP, 0U, &value);
		zassert_ok(rc, "iter %d second temperature read failed: %d", i, rc);
		zassert_within(sensor_value_to_micro(&value), 31500000LL, TELEM_TOLERANCE_MICRO);
	}
}

ZTEST(bh_arc_telem, test_decode_channel_mismatch_returns_enodata)
{
	struct sensor_value value;
	int rc;

	set_emulated_temp_and_wait(20.0f);

	rc = read_channel(&temp_iodev, SENSOR_CHAN_POWER, 0U, &value);
	zassert_equal(rc, -ENODATA, "expected -ENODATA, got %d", rc);
}

ZTEST(bh_arc_telem, test_decode_channel_index_mismatch_returns_enodata)
{
	struct sensor_value value;
	int rc;

	rc = read_channel(&tdp_iodev, SENSOR_CHAN_POWER, 1U, &value);
	zassert_equal(rc, -ENODATA, "expected -ENODATA, got %d", rc);
}

ZTEST(bh_arc_telem, test_decode_multi_channel_progresses_fit)
{
	const struct sensor_decoder_api *decoder;
	struct sensor_chan_spec temp_chan = {
		.chan_type = SENSOR_CHAN_DIE_TEMP,
		.chan_idx = 0,
	};
	struct sensor_chan_spec power_chan = {
		.chan_type = SENSOR_CHAN_POWER,
		.chan_idx = 0,
	};
	struct sensor_value temp_value;
	struct sensor_value power_value;
	size_t base_size;
	size_t frame_size;
	uint8_t read_buf[128];
	uint32_t fit = 0U;
	int rc;

	rc = sensor_get_decoder(smc_sensor, &decoder);
	zassert_ok(rc, "get decoder failed: %d", rc);

	rc = decoder->get_size_info(temp_chan, &base_size, &frame_size);
	zassert_ok(rc, "get size info failed: %d", rc);

	size_t read_size = base_size + frame_size;

	zassert_true(read_size <= sizeof(read_buf), "buffer too small");

	rc = sensor_read(&temp_tdp_iodev, &test_rtio, read_buf, read_size);
	zassert_ok(rc, "multi-channel read failed: %d", rc);

	rc = decoder->decode(read_buf, temp_chan, &fit, 2U, &temp_value);
	zassert_equal(rc, 1, "expected 1 decoded temp frame, got %d", rc);

	rc = decoder->decode(read_buf, power_chan, &fit, 2U, &power_value);
	zassert_equal(rc, 1, "expected 1 decoded power frame, got %d", rc);
}

ZTEST_SUITE(bh_arc_telem, NULL, bh_arc_telem_setup, NULL, NULL, NULL);
