/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT tenstorrent_grendel_i3c_controller

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i3c.h>
#include <zephyr/drivers/i3c/devicetree.h>
#include <zephyr/drivers/misc/tt_grendel_i3c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(tt_grendel_i3c_controller, CONFIG_TT_GRENDEL_I3C_CONTROLLER_LOG_LEVEL);

BUILD_ASSERT(CONFIG_TT_GRENDEL_I3C_CONTROLLER_INIT_PRIO > CONFIG_I3C_CONTROLLER_INIT_PRIORITY,
	     "TT_GRENDEL_I3C_CONTROLLER_INIT_PRIO must be higher than "
	     "I3C_CONTROLLER_INIT_PRIORITY so the I3C bus is ready");

/*
 * Gap between response reads; the target runs the command on a workqueue, not
 * inline. Each read drives target-side ISR work, so reading tightly would slow
 * down the very command being waited on.
 */
#define TT_GRENDEL_I3C_POLL_INTERVAL K_MSEC(100)

/* Gap between DAA retries while waiting for the target to join the bus. */
#define TT_GRENDEL_I3C_DAA_RETRY_INTERVAL K_MSEC(50)

/* A ping is answered immediately, so it needs no room for real work. */
#define TT_GRENDEL_I3C_PING_TIMEOUT K_MSEC(100)

struct tt_grendel_i3c_controller_config {
	struct i3c_device_desc *i3c_dev;
};

/*
 * The descriptor in config is only a template: DAA updates the one the bus
 * owns, so look that up by PID rather than transferring to a stale address.
 */
static struct i3c_device_desc *
tt_grendel_i3c_controller_target_get(const struct tt_grendel_i3c_controller_config *config)
{
	const struct i3c_device_id id = {.pid = config->i3c_dev->pid};

	return i3c_device_find(config->i3c_dev->bus, &id);
}

/* Collect the response the target left behind, if it is the one we asked for. */
static int tt_grendel_i3c_read_response(struct i3c_device_desc *target, uint8_t opcode,
					struct tt_grendel_i3c_response *resp)
{
	uint8_t buf[sizeof(*resp)] = {0};
	int ret;

	ret = i3c_read(target, buf, sizeof(buf));
	if (ret != 0) {
		/* Expected while the target is still booting. */
		LOG_DBG("I3C response read failed: %d", ret);
		return -EIO;
	}

	resp->result = buf[0];
	resp->opcode = buf[1];

	if (resp->opcode != opcode) {
		LOG_DBG("response for 0x%02x, waiting on 0x%02x", resp->opcode, opcode);
		return -EAGAIN;
	}

	return 0;
}

/* Internal helper; callers get a function per command instead of raw opcodes. */
static int tt_grendel_i3c_run_cmd(const struct device *dev, uint8_t opcode, const void *payload,
				  size_t payload_len, k_timeout_t timeout);

int tt_grendel_i3c_wait_ready(const struct device *dev, k_timeout_t timeout)
{
	const struct tt_grendel_i3c_controller_config *config = dev->config;
	k_timepoint_t deadline = sys_timepoint_calc(timeout);
	int ret;

	while (true) {
		k_sleep(TT_GRENDEL_I3C_DAA_RETRY_INTERVAL);

		/* The target only answers once it has joined the bus with a dynamic
		 * address, so redo DAA before every attempt rather than just once.
		 */
		i3c_do_daa(config->i3c_dev->bus);

		ret = tt_grendel_i3c_run_cmd(dev, TT_GRENDEL_I3C_CMD_PING, NULL, 0,
					     TT_GRENDEL_I3C_PING_TIMEOUT);
		if (ret == 0) {
			break;
		}

		if (sys_timepoint_expired(deadline)) {
			LOG_ERR("remote target never answered: %d", ret);
			return -ETIMEDOUT;
		}
	}

	LOG_INF("remote target ready");

	return 0;
}

/* Internal helper; callers get a function per command instead of raw opcodes. */
static int tt_grendel_i3c_run_cmd(const struct device *dev, uint8_t opcode, const void *payload,
				  size_t payload_len, k_timeout_t timeout)
{
	const struct tt_grendel_i3c_controller_config *config = dev->config;
	struct tt_grendel_i3c_cmd_frame frame = {.opcode = opcode};
	k_timepoint_t deadline = sys_timepoint_calc(timeout);
	struct tt_grendel_i3c_response resp;
	struct i3c_device_desc *target;
	int ret;

	__ASSERT_NO_MSG(payload_len <= sizeof(frame.payload));

	target = tt_grendel_i3c_controller_target_get(config);
	if (target == NULL) {
		return -ENODEV;
	}

	if (payload_len > 0) {
		memcpy(frame.payload, payload, payload_len);
	}

	ret = i3c_write(target, (const uint8_t *)&frame, sizeof(frame));
	if (ret != 0) {
		LOG_ERR("I3C write of command 0x%02x failed: %d", opcode, ret);
		return -EIO;
	}

	while (true) {
		k_sleep(TT_GRENDEL_I3C_POLL_INTERVAL);

		if (tt_grendel_i3c_read_response(target, opcode, &resp) == 0) {
			break;
		}

		if (sys_timepoint_expired(deadline)) {
			LOG_ERR("no response to command 0x%02x before timeout", opcode);
			return -ETIMEDOUT;
		}
	}

	if (resp.result != TT_GRENDEL_I3C_OK) {
		LOG_ERR("target reported failure for command 0x%02x", opcode);
		return -EIO;
	}

	return 0;
}

int tt_grendel_i3c_d2d_init(const struct device *dev, uint8_t instance, k_timeout_t timeout)
{
	struct tt_grendel_i3c_d2d_req req = {.instance = instance};

	return tt_grendel_i3c_run_cmd(dev, TT_GRENDEL_I3C_CMD_D2D_INIT, &req, sizeof(req), timeout);
}

int tt_grendel_i3c_d2d_start(const struct device *dev, uint8_t instance, k_timeout_t timeout)
{
	struct tt_grendel_i3c_d2d_req req = {.instance = instance};

	return tt_grendel_i3c_run_cmd(dev, TT_GRENDEL_I3C_CMD_D2D_START, &req, sizeof(req),
				      timeout);
}

static int tt_grendel_i3c_controller_init(const struct device *dev)
{
	const struct tt_grendel_i3c_controller_config *cfg = dev->config;

	if (!device_is_ready(cfg->i3c_dev->bus)) {
		LOG_ERR("I3C bus %s is not ready", cfg->i3c_dev->bus->name);
		return -ENODEV;
	}

	return 0;
}

#define TT_GRENDEL_I3C_CONTROLLER_DEFINE(inst)                                                     \
	static struct i3c_device_desc tt_grendel_i3c_controller_i3c_dev_##inst[] = {               \
		I3C_DEVICE_DESC_DT(DT_INST_PHANDLE(inst, i3c_device))};                            \
	static const struct tt_grendel_i3c_controller_config                                       \
		tt_grendel_i3c_controller_config_##inst = {                                        \
			.i3c_dev = tt_grendel_i3c_controller_i3c_dev_##inst,                       \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, tt_grendel_i3c_controller_init, NULL, NULL,                    \
			      &tt_grendel_i3c_controller_config_##inst, POST_KERNEL,               \
			      CONFIG_TT_GRENDEL_I3C_CONTROLLER_INIT_PRIO, NULL);

DT_INST_FOREACH_STATUS_OKAY(TT_GRENDEL_I3C_CONTROLLER_DEFINE)
