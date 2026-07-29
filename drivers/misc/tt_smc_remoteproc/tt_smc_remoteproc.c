/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i3c.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(tt_smc_remoteproc, CONFIG_KERNEL_LOG_LEVEL);

#include <tenstorrent/occp.h>

/* For now, wait up to 1 second for remote SMC to boot. We can adjust this post silicon. */
#define SMC_BOOT_TIMEOUT_MS 1000

#define DT_DRV_COMPAT tenstorrent_smc_remoteproc

BUILD_ASSERT(CONFIG_TT_SMC_REMOTEPROC_INIT_PRIO > CONFIG_I3C_CONTROLLER_INIT_PRIORITY,
	     "TT_SMC_REMOTEPROC_INIT_PRIO must be higher than I3C_CONTROLLER_INIT_PRIORITY");

struct tt_smc_remoteproc_config {
	const struct gpio_dt_spec boot_gpio; /* Optional GPIO to signal remote ROM is ready */
};

struct tt_smc_remoteproc_data {
	struct i3c_device_desc *i3c_dev;      /* I3C device descriptor for remote SMC */
	struct occp_backend_i3c occp_backend; /* OCCP I3C backend */
};

int tt_smc_remoteproc_boot(const struct device *dev, uint64_t addr, uint8_t *img_data,
			   size_t img_size)
{
	struct tt_smc_remoteproc_data *data = dev->data;
	int ret;

	/* Write image to target */
	ret = occp_write_data(&data->occp_backend.base, addr, img_data, img_size);
	if (ret != 0) {
		LOG_ERR("Failed to write image to remote SMC: %d", ret);
		return ret;
	}

	/* Execute image on CPU 0 */
	ret = occp_execute_image(&data->occp_backend.base, addr, 0);
	if (ret != 0) {
		LOG_ERR("Failed to execute image on remote SMC: %d", ret);
		return ret;
	}

	return 0;
}

int tt_smc_remoteproc_init(const struct device *dev)
{
	const struct tt_smc_remoteproc_config *config = dev->config;
	struct tt_smc_remoteproc_data *data = dev->data;
	struct i3c_device_id remote_id = {
		.pid = data->i3c_dev->pid,
	};
	bool has_boot_gpio = config->boot_gpio.port != NULL;
	int ret;
	k_timepoint_t timeout;

	if (has_boot_gpio) {
		if (!device_is_ready(config->boot_gpio.port)) {
			return -ENODEV;
		}

		gpio_pin_configure_dt(&config->boot_gpio, GPIO_INPUT);

		timeout = sys_timepoint_calc(K_MSEC(SMC_BOOT_TIMEOUT_MS));
		/* Wait for remote GPIO to rise */
		while (gpio_pin_get_dt(&config->boot_gpio) == 0) {
			/* wait */
			if (sys_timepoint_expired(timeout)) {
				LOG_ERR("Timeout waiting for remote SMC ROM to be ready");
				return -ETIMEDOUT;
			}
		}

		LOG_INF("Remote SMC ROM is ready");

		/*
		 * Remote SMC is now on the I3C bus, run dynamic address assignment
		 * once more so that the controller discovers it
		 */

		ret = i3c_do_daa(data->i3c_dev->bus);
		if (ret != 0) {
			LOG_INF("I3C dynamic address assignment failed: %d", ret);
			return ret;
		}
	} else {
		LOG_INF("boot-gpios not configured; skipping GPIO wait and DAA");
	}

	/* Now that DAA is complete, find the device on the bus */
	data->i3c_dev = i3c_device_find(data->i3c_dev->bus, &remote_id);
	if (!data->i3c_dev) {
		LOG_INF("Remote SMC device not found on I3C bus");
		return -ENODEV;
	}

	LOG_INF("Remote SMC device found at dynamic address 0x%02x", data->i3c_dev->dynamic_addr);

	/* Initialize OCCP I3C backend */
	ret = occp_backend_i3c_init(&data->occp_backend, data->i3c_dev);
	if (ret != 0) {
		LOG_ERR("Failed to initialize OCCP I3C backend: %d", ret);
		return ret;
	}

	return 0;
}

#define TT_SMC_REMOTEPROC_DEFINE(inst)                                                             \
	const struct tt_smc_remoteproc_config tt_smc_remoteproc_config_##inst = {                  \
		.boot_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, boot_gpios, {0}),                      \
	};                                                                                         \
                                                                                                   \
	struct i3c_device_desc tt_smc_remoteproc_i3c_dev_##inst[] = {                              \
		I3C_DEVICE_DESC_DT_INST(inst)};                                                    \
                                                                                                   \
	struct tt_smc_remoteproc_data tt_smc_remoteproc_data_##inst = {                            \
		.i3c_dev = tt_smc_remoteproc_i3c_dev_##inst,                                       \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, tt_smc_remoteproc_init, NULL, &tt_smc_remoteproc_data_##inst,  \
			      &tt_smc_remoteproc_config_##inst, POST_KERNEL,                       \
			      CONFIG_TT_SMC_REMOTEPROC_INIT_PRIO, NULL);

DT_INST_FOREACH_STATUS_OKAY(TT_SMC_REMOTEPROC_DEFINE)
