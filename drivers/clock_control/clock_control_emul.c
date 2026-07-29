/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Clock control emulation driver for native simulation
 */

#define DT_DRV_COMPAT tenstorrent_clock_control_emul
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(clock_control_emul, CONFIG_CLOCK_CONTROL_LOG_LEVEL);

struct clock_control_emul_data {
	uint32_t clock_rates[16]; /* Support up to 16 different clocks */
	bool clock_enabled[16];   /* Track enabled state for each clock */
};

struct clock_control_emul_config {
	uint32_t default_rate;
};

static int clock_control_emul_on(const struct device *dev, clock_control_subsys_t sys)
{
	struct clock_control_emul_data *data = dev->data;
	uintptr_t subsys_id = (uintptr_t)sys;

	if (subsys_id >= ARRAY_SIZE(data->clock_rates)) {
		LOG_ERR("Invalid subsys ID %lu", subsys_id);
		return -EINVAL;
	}

	data->clock_enabled[subsys_id] = true;
	LOG_DBG("Clock ON for subsys %lu", subsys_id);
	return 0;
}

static int clock_control_emul_off(const struct device *dev, clock_control_subsys_t sys)
{
	struct clock_control_emul_data *data = dev->data;
	uintptr_t subsys_id = (uintptr_t)sys;

	if (subsys_id >= ARRAY_SIZE(data->clock_rates)) {
		LOG_ERR("Invalid subsys ID %lu", subsys_id);
		return -EINVAL;
	}

	data->clock_enabled[subsys_id] = false;
	LOG_DBG("Clock OFF for subsys %lu", subsys_id);
	return 0;
}

static int clock_control_emul_get_rate(const struct device *dev, clock_control_subsys_t sys,
				       uint32_t *rate)
{
	struct clock_control_emul_data *data = dev->data;
	const struct clock_control_emul_config *config = dev->config;
	uintptr_t subsys_id = (uintptr_t)sys;

	if (subsys_id >= ARRAY_SIZE(data->clock_rates)) {
		LOG_ERR("Invalid subsys ID %lu", subsys_id);
		return -EINVAL;
	}

	if (data->clock_rates[subsys_id] == 0) {
		*rate = config->default_rate;
	} else {
		*rate = data->clock_rates[subsys_id];
	}

	LOG_DBG("Get rate for subsys %lu: %u Hz", subsys_id, *rate);
	return 0;
}

static int clock_control_emul_set_rate(const struct device *dev, clock_control_subsys_t sys,
				       clock_control_subsys_rate_t rate)
{
	struct clock_control_emul_data *data = dev->data;
	uintptr_t subsys_id = (uintptr_t)sys;
	uint32_t new_rate = (uint32_t)(uintptr_t)rate;

	if (subsys_id >= ARRAY_SIZE(data->clock_rates)) {
		LOG_ERR("Invalid subsys ID %lu", subsys_id);
		return -EINVAL;
	}

	data->clock_rates[subsys_id] = new_rate;
	LOG_DBG("Set rate for subsys %lu: %u Hz", subsys_id, new_rate);
	return 0;
}

static enum clock_control_status clock_control_emul_get_status(const struct device *dev,
							       clock_control_subsys_t sys)
{
	struct clock_control_emul_data *data = dev->data;
	uintptr_t subsys_id = (uintptr_t)sys;

	if (subsys_id >= ARRAY_SIZE(data->clock_rates)) {
		LOG_ERR("Invalid subsys ID %lu", subsys_id);
		return CLOCK_CONTROL_STATUS_UNKNOWN;
	}

	return data->clock_enabled[subsys_id] ? CLOCK_CONTROL_STATUS_ON : CLOCK_CONTROL_STATUS_OFF;
}

static DEVICE_API(clock_control, clock_control_emul_api) = {
	.on = clock_control_emul_on,
	.off = clock_control_emul_off,
	.get_rate = clock_control_emul_get_rate,
	.set_rate = clock_control_emul_set_rate,
	.get_status = clock_control_emul_get_status,
};

static int clock_control_emul_init(const struct device *dev)
{
	struct clock_control_emul_data *data = dev->data;
	const struct clock_control_emul_config *config = dev->config;

	/* Initialize all clock rates to default and enabled */
	for (int i = 0; i < ARRAY_SIZE(data->clock_rates); i++) {
		data->clock_rates[i] = config->default_rate;
		data->clock_enabled[i] = true;
	}

	LOG_DBG("Clock control emulator initialized with default rate %u Hz", config->default_rate);
	return 0;
}

#define CLOCK_CONTROL_EMUL_DEVICE(inst)                                                            \
	static struct clock_control_emul_data clock_control_emul_data_##inst;                      \
                                                                                                   \
	static const struct clock_control_emul_config clock_control_emul_config_##inst = {         \
		.default_rate = DT_INST_PROP_OR(inst, default_rate, 1000000000),                   \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, clock_control_emul_init, NULL,                                 \
			      &clock_control_emul_data_##inst, &clock_control_emul_config_##inst,  \
			      PRE_KERNEL_1, CONFIG_CLOCK_CONTROL_INIT_PRIORITY,                    \
			      &clock_control_emul_api);

DT_INST_FOREACH_STATUS_OKAY(CLOCK_CONTROL_EMUL_DEVICE)
