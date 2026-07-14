/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_dummy_sensor_emul

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

static DEVICE_API(sensor, pldm_pdr_dummy_sensor_api) = {
	.sample_fetch = NULL,
	.channel_get = NULL,
};

static int pldm_pdr_dummy_sensor_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define PLDM_PDR_DUMMY_SENSOR_DEFINE(inst)                                                         \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, pldm_pdr_dummy_sensor_init, NULL, NULL, NULL,           \
				     POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,                     \
				     &pldm_pdr_dummy_sensor_api)

DT_INST_FOREACH_STATUS_OKAY(PLDM_PDR_DUMMY_SENSOR_DEFINE)
