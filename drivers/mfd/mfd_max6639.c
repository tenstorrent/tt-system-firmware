/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT maxim_max6639

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mfd/max6639.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(max6639_mfd, CONFIG_MFD_LOG_LEVEL);

struct max6639_config {
	struct i2c_dt_spec i2c;
};

static int max6639_init(const struct device *dev)
{
	const struct max6639_config *config = dev->config;
	int result;

	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR("I2C device not ready");
		return -ENODEV;
	}

	/* enable PWM manual mode, RPM to max */
	result = i2c_reg_write_byte_dt(&config->i2c, MAX6639_REG_CHANNEL_1_CONFIG_1, 0x82);
	if (result != 0) {
		return result;
	}
	result = i2c_reg_write_byte_dt(&config->i2c, MAX6639_REG_CHANNEL_2_CONFIG_1, 0x82);
	if (result != 0) {
		return result;
	}

	/* select high PWM frequency output range */
	result = i2c_reg_write_byte_dt(&config->i2c, MAX6639_REG_GLOBAL_CONFIG, 0x38);
	if (result != 0) {
		return result;
	}

	/* disable pulse stretching, deassert THERM, set PWM frequency to high */
	result = i2c_reg_write_byte_dt(&config->i2c, MAX6639_REG_CHANNEL_1_CONFIG_3, 0x23);
	if (result != 0) {
		return result;
	}
	result = i2c_reg_write_byte_dt(&config->i2c, MAX6639_REG_CHANNEL_2_CONFIG_3, 0x23);
	if (result != 0) {
		return result;
	}

	result = i2c_reg_write_byte_dt(&config->i2c, MAX6639_REG_FAN_1_PPR, 0x40);
	if (result != 0) {
		return result;
	}

	result = i2c_reg_write_byte_dt(&config->i2c, MAX6639_REG_FAN_2_PPR, 0x40);
	if (result != 0) {
		return result;
	}

	return 0;
}

#define MAX6639_INIT(inst)                                                                         \
	static const struct max6639_config max6639_##inst##_config = {                             \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, max6639_init, NULL, NULL, &max6639_##inst##_config,            \
			      POST_KERNEL, CONFIG_MFD_MAX6639_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(MAX6639_INIT);
