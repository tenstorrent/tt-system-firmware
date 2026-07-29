/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <errno.h>

#ifdef CONFIG_I3C_TARGET
#include <zephyr/drivers/i3c.h>
#include <zephyr/drivers/i3c/target_device.h>
#endif

LOG_MODULE_REGISTER(dm_test_app, LOG_LEVEL_INF);

#ifdef CONFIG_I3C_TARGET
static const struct device *i3c_dev = DEVICE_DT_GET(DT_NODELABEL(i3c1));

static uint8_t value;

static int target_prefill(void)
{
	return i3c_target_tx_write(i3c_dev, NULL, 8U, I3C_MSG_HDR_MODE0);
}

/* I3C target callback functions */
static int i3c_target_write_requested_cb(struct i3c_target_config *config)
{
	ARG_UNUSED(config);
	LOG_INF("I3C Target: Write requested callback entered");
	return 0;
}

static int i3c_target_write_received_cb(struct i3c_target_config *config, uint8_t val)
{
	ARG_UNUSED(config);
	LOG_INF("I3C Target: Write received callback entered, received: 0x%02x", val);
	return 0;
}

static int i3c_target_read_requested_cb(struct i3c_target_config *config, uint8_t *val)
{
	ARG_UNUSED(config);
	ARG_UNUSED(val);
	target_prefill();
	return 0;
}

static int i3c_target_read_processed_cb(struct i3c_target_config *config, uint8_t *val)
{
	ARG_UNUSED(config);
	*val = value++; /* Return dummy data */
	return 0;
}

static int i3c_target_stop_cb(struct i3c_target_config *config)
{
	ARG_UNUSED(config);
	LOG_INF("I3C Target: Stop callback entered");
	return 0;
}

/* I3C target callbacks structure */
static const struct i3c_target_callbacks i3c_target_callbacks = {
	.write_requested_cb = i3c_target_write_requested_cb,
	.write_received_cb = i3c_target_write_received_cb,
	.read_requested_cb = i3c_target_read_requested_cb,
	.read_processed_cb = i3c_target_read_processed_cb,
	.stop_cb = i3c_target_stop_cb,
};

/* I3C target configuration */
static struct i3c_target_config i3c_target_config = {
	.callbacks = &i3c_target_callbacks,
};

#endif

int main(void)
{
	LOG_INF("Hello World! This is dm_test_app running on %s", CONFIG_BOARD);

#ifdef CONFIG_I3C_TARGET
	int ret;

	if (!device_is_ready(i3c_dev)) {
		LOG_ERR("I3C device is not ready");
		return -ENODEV;
	}

	/* Register I3C target with address 0 */
	ret = i3c_target_register(i3c_dev, &i3c_target_config);
	if (ret < 0) {
		LOG_ERR("Failed to register I3C target: %d", ret);
		return ret;
	}

	target_prefill();

	LOG_INF("I3C target registered successfully");
#endif

	while (1) {
		k_sleep(K_SECONDS(5));
	}

	return 0;
}
