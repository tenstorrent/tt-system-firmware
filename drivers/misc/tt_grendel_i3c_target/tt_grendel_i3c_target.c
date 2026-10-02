/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT tenstorrent_grendel_i3c_target

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i3c.h>
#include <zephyr/drivers/i3c/target_device.h>
#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/drivers/misc/tt_d2d.h>
#include <zephyr/drivers/misc/tt_grendel_i3c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>

LOG_MODULE_REGISTER(tt_grendel_i3c_target, CONFIG_TT_GRENDEL_I3C_TARGET_LOG_LEVEL);

BUILD_ASSERT(CONFIG_TT_GRENDEL_I3C_TARGET_INIT_PRIO > CONFIG_TT_D2D_INIT_PRIO,
	     "TT_GRENDEL_I3C_TARGET_INIT_PRIO must be higher than TT_D2D_INIT_PRIO");

/*
 * The firmware image and a little-endian uint32_t length header in front of it
 * are written into local memory by the controller die over OCCP before this
 * driver runs, so a D2D_INIT command carries no payload.
 */
#define TT_GRENDEL_I3C_D2D_FW_ADDR TT_D2D_OCCP_STAGE_ADDR

struct tt_grendel_i3c_target_config {
	const struct device *i3c;
	const struct device *const *d2ds;
	size_t num_d2ds;
};

struct tt_grendel_i3c_target_data {
	const struct device *dev;
	struct i3c_target_config target_cfg;
	struct k_work work;

	/* Command assembly, ISR context only. */
	struct tt_grendel_i3c_cmd_frame rx_frame;
	uint8_t rx_len;

	/* Response handed back to the controller, written from the workqueue. */
	struct tt_grendel_i3c_response tx_resp;

	/* Opcode and payload handed to the workqueue. */
	uint8_t pending_opcode;
	uint8_t pending_payload[TT_GRENDEL_I3C_CMD_PAYLOAD_MAX_LEN];
};

static int tt_grendel_i3c_handle_d2d_init(const struct tt_grendel_i3c_target_config *cfg,
					  const struct tt_grendel_i3c_d2d_req *req)
{
	uint32_t fw_size = sys_le32_to_cpu(sys_read32(TT_GRENDEL_I3C_D2D_FW_ADDR));
	uint8_t instance = req->instance;
	int ret;

	if (instance >= cfg->num_d2ds) {
		LOG_ERR("D2D_INIT: instance %u out of range (%zu tile(s))", instance,
			cfg->num_d2ds);
		return -EINVAL;
	}

	LOG_INF("D2D_INIT: loading D2D FW (%u bytes) onto tile %u", fw_size, instance);

	ret = tt_d2d_reset_release(cfg->d2ds[instance]);
	if (ret != 0) {
		LOG_ERR("Failed to release D2D[%u] reset: %d", instance, ret);
		return ret;
	}

	ret = tt_d2d_load_fw(cfg->d2ds[instance],
			     (const uint8_t *)(TT_GRENDEL_I3C_D2D_FW_ADDR + TT_D2D_FW_HEADER_SIZE),
			     fw_size);
	if (ret != 0) {
		LOG_ERR("Failed to load D2D[%u] FW: %d", instance, ret);
		return ret;
	}
	LOG_INF("D2D_INIT: Done loading D2D FW");

	return 0;
}

static int tt_grendel_i3c_handle_d2d_start(const struct tt_grendel_i3c_target_config *cfg,
					   const struct tt_grendel_i3c_d2d_req *req)
{
	uint8_t instance = req->instance;

	if (instance >= cfg->num_d2ds) {
		LOG_ERR("D2D_START: instance %u out of range (%zu tile(s))", instance,
			cfg->num_d2ds);
		return -EINVAL;
	}

	LOG_INF("D2D_START: releasing D2D tile %u", instance);

	return tt_d2d_start(cfg->d2ds[instance]);
}

/* Opcode dispatch. Adding a command means adding an entry and its handler. */
static int tt_grendel_i3c_dispatch(const struct tt_grendel_i3c_target_config *cfg, uint8_t opcode,
				   const uint8_t *payload)
{
	switch (opcode) {
	case TT_GRENDEL_I3C_CMD_PING:
		return 0;
	case TT_GRENDEL_I3C_CMD_D2D_INIT:
		return tt_grendel_i3c_handle_d2d_init(
			cfg, (const struct tt_grendel_i3c_d2d_req *)payload);
	case TT_GRENDEL_I3C_CMD_D2D_START:
		return tt_grendel_i3c_handle_d2d_start(
			cfg, (const struct tt_grendel_i3c_d2d_req *)payload);
	default:
		return -ENOTSUP;
	}
}

static void tt_grendel_i3c_target_work(struct k_work *work)
{
	struct tt_grendel_i3c_target_data *data =
		CONTAINER_OF(work, struct tt_grendel_i3c_target_data, work);
	const struct tt_grendel_i3c_target_config *cfg = data->dev->config;
	uint8_t opcode = data->pending_opcode;
	int ret;

	ret = tt_grendel_i3c_dispatch(cfg, opcode, data->pending_payload);
	if (ret != 0) {
		LOG_ERR("command 0x%02x failed: %d", opcode, ret);
	}

	data->tx_resp.result = (ret == 0) ? TT_GRENDEL_I3C_OK : TT_GRENDEL_I3C_NOT_OK;
	data->tx_resp.opcode = opcode;

	/* hdr_mode 0 means plain SDR; I3C_MSG_HDR_MODE0 is HDR-DDR and unsupported here. */
	ret = i3c_target_tx_write(cfg->i3c, (uint8_t *)&data->tx_resp, sizeof(data->tx_resp), 0);
	if (ret < 0) {
		LOG_WRN("Failed to answer command 0x%02x: %d", opcode, ret);
	}
}

/* Dispatch once a whole command frame has arrived; the IP gives us no start callback. */
static int tt_grendel_i3c_write_received(struct i3c_target_config *target_cfg, uint8_t val)
{
	struct tt_grendel_i3c_target_data *data =
		CONTAINER_OF(target_cfg, struct tt_grendel_i3c_target_data, target_cfg);

	LOG_DBG("write received: 0x%02x (byte %u)", val, data->rx_len);

	((uint8_t *)&data->rx_frame)[data->rx_len++] = val;

	if (data->rx_len < sizeof(data->rx_frame)) {
		return 0;
	}

	data->rx_len = 0;
	data->pending_opcode = data->rx_frame.opcode;
	memcpy(data->pending_payload, data->rx_frame.payload, sizeof(data->pending_payload));

	LOG_DBG("command 0x%02x", data->pending_opcode);
	k_work_submit(&data->work);

	return 0;
}

static int tt_grendel_i3c_stop(struct i3c_target_config *target_cfg)
{
	struct tt_grendel_i3c_target_data *data =
		CONTAINER_OF(target_cfg, struct tt_grendel_i3c_target_data, target_cfg);

	data->rx_len = 0;

	return 0;
}

static const struct i3c_target_callbacks tt_grendel_i3c_target_callbacks = {
	.write_received_cb = tt_grendel_i3c_write_received,
	.stop_cb = tt_grendel_i3c_stop,
};

static int tt_grendel_i3c_target_init(const struct device *dev)
{
	const struct tt_grendel_i3c_target_config *cfg = dev->config;
	struct tt_grendel_i3c_target_data *data = dev->data;
	int ret;

	if (!device_is_ready(cfg->i3c)) {
		LOG_ERR("I3C device %s is not ready", cfg->i3c->name);
		return -ENODEV;
	}

	data->dev = dev;
	data->target_cfg.callbacks = &tt_grendel_i3c_target_callbacks;
	k_work_init(&data->work, tt_grendel_i3c_target_work);

	ret = i3c_target_register(cfg->i3c, &data->target_cfg);
	if (ret != 0) {
		LOG_ERR("Failed to register as an I3C target on %s: %d", cfg->i3c->name, ret);
		return ret;
	}

	LOG_INF("Awaiting commands on %s for %zu D2D tile(s)", cfg->i3c->name, cfg->num_d2ds);

	return 0;
}

#define TT_GRENDEL_I3C_TARGET_GET(node_id, prop, idx)                                              \
	DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)),

#define TT_GRENDEL_I3C_TARGET_DEFINE(inst)                                                         \
	static const struct device *const tt_grendel_i3c_target_d2ds_##inst[] = {                  \
		DT_INST_FOREACH_PROP_ELEM(inst, d2d, TT_GRENDEL_I3C_TARGET_GET)};                  \
	static const struct tt_grendel_i3c_target_config tt_grendel_i3c_target_config_##inst = {   \
		.i3c = DEVICE_DT_GET(DT_INST_PHANDLE(inst, i3c)),                                  \
		.d2ds = tt_grendel_i3c_target_d2ds_##inst,                                         \
		.num_d2ds = ARRAY_SIZE(tt_grendel_i3c_target_d2ds_##inst),                         \
	};                                                                                         \
	static struct tt_grendel_i3c_target_data tt_grendel_i3c_target_data_##inst;                \
	DEVICE_DT_INST_DEFINE(inst, tt_grendel_i3c_target_init, NULL,                              \
			      &tt_grendel_i3c_target_data_##inst,                                  \
			      &tt_grendel_i3c_target_config_##inst, POST_KERNEL,                   \
			      CONFIG_TT_GRENDEL_I3C_TARGET_INIT_PRIO, NULL);

DT_INST_FOREACH_STATUS_OKAY(TT_GRENDEL_I3C_TARGET_DEFINE)
