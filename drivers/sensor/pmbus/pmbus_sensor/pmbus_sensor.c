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

#define PMBUS_CMD_READ_PIN 0x97

struct pmbus_sensor_config {
	struct smbus_dt_spec smbus;
};

struct pmbus_sensor_data {
	int64_t power_microwatts;
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

	ret = pmbus_decode_linear11_micro(raw, &power_microwatts);
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
	uint32_t smbus_config = SMBUS_MODE_CONTROLLER | SMBUS_MODE_PEC;

	if (!device_is_ready(config->smbus.bus)) {
		LOG_ERR("SMBus device not ready");
		return -ENODEV;
	}

	return smbus_configure(config->smbus.bus, smbus_config);
}

static DEVICE_API(sensor, pmbus_sensor_api) = {
	.sample_fetch = pmbus_sensor_sample_fetch,
	.channel_get = pmbus_sensor_channel_get,
};

#define PMBUS_SENSOR_INIT(inst)                                                                    \
	static struct pmbus_sensor_data pmbus_sensor_data_##inst;                                  \
	static const struct pmbus_sensor_config pmbus_sensor_config_##inst = {                     \
		.smbus = SMBUS_DT_SPEC_INST_GET(inst),                                             \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, pmbus_sensor_init, NULL, &pmbus_sensor_data_##inst,            \
			      &pmbus_sensor_config_##inst, POST_KERNEL,                            \
			      CONFIG_PMBUS_SENSOR_INIT_PRIORITY, &pmbus_sensor_api);

DT_INST_FOREACH_STATUS_OKAY(PMBUS_SENSOR_INIT)
