/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT pmbus_sensor

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/smbus.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#define PMBUS_CMD_READ_PIN 0x97

/* eFuse-only registers, used when the devicetree sets dmc_efuse. */
#define DMC_EFUSE_CMD_DEVICE_CONFIG_00 0xC2
#define DMC_EFUSE_CMD_MFR_SYS_CTRL     0xCF
/* MFR_SYS_CTRL bit 12: 1 = LINEAR11 telemetry, 0 = DIRECT. */
#define DMC_EFUSE_REPORT_FORMAT_LINEAR11 BIT(12)
/* DEVICE_CONFIG_00 bits 1:0: DIRECT resolution code for IIN and PIN. */
#define DMC_EFUSE_SCALE_SEL_MASK 0x3

struct pmbus_sensor_config {
	struct smbus_dt_spec smbus;
	bool dmc_efuse;
};

/* DIRECT codec: X = Y * 10^-r / m, with the offset b always zero here. */
struct pmbus_direct_codec {
	int32_t m;
	int32_t r;
};

/* READ_PIN codecs by DMC_EFUSE SCALE_SEL: 2, 8 and 32 W per LSB. */
static const struct pmbus_direct_codec dmc_efuse_pin_direct[] = {
	{.m = 500, .r = -3},
	{.m = 125, .r = -3},
	{.m = 3125, .r = -5},
};

struct pmbus_sensor_data {
	int64_t power_microwatts;
	/* Set at init on DMC_EFUSE parts in DIRECT mode; NULL means LINEAR11. */
	const struct pmbus_direct_codec *pin_codec;
};

LOG_MODULE_REGISTER(pmbus_sensor, CONFIG_SENSOR_LOG_LEVEL);

static int pmbus_decode_linear11_micro(uint16_t raw, int64_t *value)
{
	int32_t exponent = (int32_t)(raw >> 11);
	int32_t mantissa = (int32_t)(raw & 0x7ff);

	if ((exponent & 0x10) != 0) {
		exponent -= 0x20;
	}
	if ((mantissa & 0x400) != 0) {
		mantissa -= 0x800;
	}

	if (exponent >= 0) {
		if (exponent > 43 ||
		    (mantissa > 0 && (int64_t)mantissa > INT64_MAX / (1000000LL << exponent)) ||
		    (mantissa < 0 && (int64_t)mantissa < INT64_MIN / (1000000LL << exponent))) {
			return -ERANGE;
		}
		*value = (int64_t)mantissa * (1000000LL << exponent);
	} else {
		const int32_t shift = -exponent;

		if (shift >= 63 || (mantissa > 0 && mantissa > INT64_MAX / 1000000LL) ||
		    (mantissa < 0 && mantissa < INT64_MIN / 1000000LL)) {
			*value = 0;
		} else {
			*value = ((int64_t)mantissa * 1000000LL) / (1LL << shift);
		}
	}

	return 0;
}

/* Decodes a two's-complement DIRECT word into microwatts. */
static int pmbus_decode_direct_micro(uint16_t raw, const struct pmbus_direct_codec *codec,
				     int64_t *value)
{
	int64_t scaled = (int64_t)(int16_t)raw;

	if (codec->r > 0) {
		return -EINVAL;
	}
	for (int32_t i = 0; i < -codec->r; i++) {
		scaled *= 10;
	}

	/* X in watts = Y * 10^-r / m; multiply by 1e6 for microwatts first. */
	*value = scaled * 1000000LL / codec->m;
	return 0;
}

/*
 * Reads the telemetry format once, at init. The mode is assumed to stay as
 * read: if another master or an NVM restore changes MFR_SYS_CTRL[12] or
 * DEVICE_CONFIG_00[1:0] afterwards, readings are mis-scaled until reset.
 */
static int dmc_efuse_read_mode(const struct device *dev)
{
	const struct pmbus_sensor_config *config = dev->config;
	struct pmbus_sensor_data *data = dev->data;
	uint16_t mode;
	uint16_t scale;
	int ret;

	ret = smbus_word_data_read(config->smbus.bus, config->smbus.addr,
				   DMC_EFUSE_CMD_MFR_SYS_CTRL, &mode);
	if (ret != 0) {
		return ret;
	}
	if ((mode & DMC_EFUSE_REPORT_FORMAT_LINEAR11) != 0) {
		data->pin_codec = NULL;
		return 0;
	}

	ret = smbus_word_data_read(config->smbus.bus, config->smbus.addr,
				   DMC_EFUSE_CMD_DEVICE_CONFIG_00, &scale);
	if (ret != 0) {
		return ret;
	}
	scale &= DMC_EFUSE_SCALE_SEL_MASK;
	if (scale >= ARRAY_SIZE(dmc_efuse_pin_direct)) {
		/* Code 3 is reserved and has no resolution. */
		LOG_ERR("DMC_EFUSE SCALE_SEL is reserved (3)");
		return -EINVAL;
	}

	data->pin_codec = &dmc_efuse_pin_direct[scale];
	return 0;
}

static int pmbus_sensor_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct pmbus_sensor_config *config = dev->config;
	struct pmbus_sensor_data *data = dev->data;
	uint16_t raw;
	int64_t power_microwatts;
	int ret;

	if (chan != SENSOR_CHAN_POWER && chan != SENSOR_CHAN_ALL) {
		return -EINVAL;
	}

	ret = smbus_word_data_read(config->smbus.bus, config->smbus.addr, PMBUS_CMD_READ_PIN, &raw);
	if (ret != 0) {
		return ret;
	}

	if (data->pin_codec != NULL) {
		ret = pmbus_decode_direct_micro(raw, data->pin_codec, &power_microwatts);
	} else {
		ret = pmbus_decode_linear11_micro(raw, &power_microwatts);
	}
	if (ret != 0) {
		return ret;
	}

	data->power_microwatts = power_microwatts;
	return 0;
}

static int pmbus_sensor_channel_get(const struct device *dev, enum sensor_channel chan,
				    struct sensor_value *val)
{
	const struct pmbus_sensor_data *data = dev->data;

	if (chan != SENSOR_CHAN_POWER) {
		return -EINVAL;
	}

	return sensor_value_from_micro(val, data->power_microwatts);
}

static int pmbus_sensor_init(const struct device *dev)
{
	const struct pmbus_sensor_config *config = dev->config;
	uint32_t smbus_config = SMBUS_MODE_CONTROLLER;
	int ret;

	if (!device_is_ready(config->smbus.bus)) {
		LOG_ERR("SMBus device not ready");
		return -ENODEV;
	}

	ret = smbus_configure(config->smbus.bus, smbus_config);
	if (ret != 0) {
		return ret;
	}

	if (config->dmc_efuse) {
		ret = dmc_efuse_read_mode(dev);
		if (ret != 0) {
			LOG_ERR("DMC_EFUSE telemetry mode read failed: %d", ret);
		}
		return ret;
	}

	return 0;
}

static DEVICE_API(sensor, pmbus_sensor_api) = {
	.sample_fetch = pmbus_sensor_sample_fetch,
	.channel_get = pmbus_sensor_channel_get,
};

#define PMBUS_SENSOR_INIT(inst)                                                                    \
	static struct pmbus_sensor_data pmbus_sensor_data_##inst;                                  \
	static const struct pmbus_sensor_config pmbus_sensor_config_##inst = {                                                                \
			.dmc_efuse = DT_INST_PROP(inst, dmc_efuse),                    \
		.smbus = SMBUS_DT_SPEC_INST_GET(inst),                                             \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, pmbus_sensor_init, NULL, &pmbus_sensor_data_##inst,            \
			      &pmbus_sensor_config_##inst, POST_KERNEL,                            \
			      CONFIG_PMBUS_SENSOR_INIT_PRIORITY, &pmbus_sensor_api);

DT_INST_FOREACH_STATUS_OKAY(PMBUS_SENSOR_INIT)
