/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_dummy_sensor_emul

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

static int pldm_pdr_dummy_sensor_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);
	return 0;
}

static int pldm_pdr_dummy_sensor_channel_get(const struct device *dev, enum sensor_channel chan,
					     struct sensor_value *val)
{
	ARG_UNUSED(dev);

	if (val == NULL) {
		return -EINVAL;
	}

	switch (chan) {
	case SENSOR_CHAN_DIE_TEMP:
		/* 12 C maps to PLDM reading 120 with unit_modifier = -1. */
		val->val1 = 12;
		val->val2 = 0;
		return 0;
	case SENSOR_CHAN_POWER:
		val->val1 = 45;
		val->val2 = 0;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static DEVICE_API(sensor, pldm_pdr_dummy_sensor_api) = {
	.sample_fetch = pldm_pdr_dummy_sensor_sample_fetch,
	.channel_get = pldm_pdr_dummy_sensor_channel_get,
};

#define PLDM_PDR_DUMMY_SENSOR_DEFINE(inst)                                                         \
	static int pldm_pdr_dummy_sensor_init_##inst(const struct device *dev)                     \
	{                                                                                          \
		ARG_UNUSED(dev);                                                                   \
		if (DT_INST_PROP_OR(inst, force_init_fail, 0)) {                                   \
			return -EIO;                                                               \
		}                                                                                  \
		return 0;                                                                          \
	}                                                                                          \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, pldm_pdr_dummy_sensor_init_##inst, NULL, NULL, NULL,    \
				     POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,                     \
				     &pldm_pdr_dummy_sensor_api)

DT_INST_FOREACH_STATUS_OKAY(PLDM_PDR_DUMMY_SENSOR_DEFINE)
