/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_tt_occp_i3c

#include <zephyr/device.h>
#include <zephyr/drivers/i3c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <tenstorrent/occp.h>
#include <zephyr/drivers/misc/tt_occp_i3c.h>

BUILD_ASSERT(CONFIG_TT_OCCP_I3C_INIT_PRIORITY > CONFIG_I3C_CONTROLLER_INIT_PRIORITY,
	     "TT_OCCP_I3C_INIT_PRIORITY must be higher than I3C_CONTROLLER_INIT_PRIORITY");

LOG_MODULE_REGISTER(tt_occp_i3c, CONFIG_TT_OCCP_I3C_LOG_LEVEL);

struct tt_occp_i3c_config {
	const struct device *i3c_dev;
	struct i3c_device_desc *i3c_device_template;
};

struct tt_occp_i3c_data {
	struct occp_backend_i3c occp_backend;
	bool initialized;
};

static int tt_occp_i3c_init(const struct device *dev)
{
	const struct tt_occp_i3c_config *config = dev->config;
	struct tt_occp_i3c_data *data = dev->data;
	struct i3c_device_desc *i3c_device;
	struct i3c_device_id i3c_id;
	int ret;

	LOG_DBG("Initializing TT OCCP I3C device");

	/* Verify I3C controller is ready */

	if (!device_is_ready(config->i3c_dev)) {
		LOG_ERR("I3C controller device not ready");
		return -ENODEV;
	}

	if (config->i3c_device_template == NULL) {
		LOG_ERR("I3C target descriptor not configured");
		return -EINVAL;
	}

	i3c_id.pid = config->i3c_device_template->pid;
	i3c_device = i3c_device_find(config->i3c_dev, &i3c_id);
	if (i3c_device == NULL) {
		LOG_ERR("I3C target descriptor not found on controller bus");
		return -ENODEV;
	}

	ret = occp_backend_i3c_init(&data->occp_backend, i3c_device);
	if (ret != 0) {
		LOG_ERR("Failed to initialize OCCP I3C backend: %d", ret);
		return ret;
	}

	data->initialized = true;
	LOG_INF("TT OCCP I3C device initialized");

	return 0;
}

const struct device *tt_occp_i3c_get_i3c_device(const struct device *dev)
{
	const struct tt_occp_i3c_config *config = dev->config;

	return config->i3c_dev;
}

bool tt_occp_i3c_is_initialized(const struct device *dev)
{
	struct tt_occp_i3c_data *data = dev->data;

	return data->initialized;
}

const struct occp_backend *tt_occp_i3c_get_backend(const struct device *dev)
{
	struct tt_occp_i3c_data *data = dev->data;

	if (!data->initialized) {
		return NULL;
	}

	return &data->occp_backend.base;
}

#define TT_OCCP_I3C_INIT(n)                                                                        \
	static struct i3c_device_desc tt_occp_i3c_i3c_dev_##n[] = {I3C_DEVICE_DESC_DT_INST(n)};    \
	static struct tt_occp_i3c_data tt_occp_i3c_data_##n;                                       \
	static const struct tt_occp_i3c_config tt_occp_i3c_config_##n = {                          \
		.i3c_dev = DEVICE_DT_GET(DT_BUS(DT_DRV_INST(n))),                                  \
		.i3c_device_template = tt_occp_i3c_i3c_dev_##n,                                    \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, tt_occp_i3c_init, NULL, &tt_occp_i3c_data_##n,                    \
			      &tt_occp_i3c_config_##n, POST_KERNEL,                                \
			      CONFIG_TT_OCCP_I3C_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(TT_OCCP_I3C_INIT)
