/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_bh_pvt_emul

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/tenstorrent/pvt_tt_bh.h>
#include <zephyr/logging/log.h>
#include <zephyr/rtio/rtio.h>

LOG_MODULE_REGISTER(pvt_tt_bh_emul, LOG_LEVEL_DBG);

struct pvt_tt_bh_emul_config {
	uint8_t num_pd;
	uint8_t num_vm;
	uint8_t num_ts;
	uint16_t pd_raw;
	uint16_t vm_raw;
	uint16_t ts_raw;
};

static int pvt_tt_bh_emul_attr_get(const struct device *dev, enum sensor_channel chan,
				   enum sensor_attribute attr, struct sensor_value *val)
{
	ARG_UNUSED(chan);

	if (!dev || !val) {
		return -EINVAL;
	}

	const struct pvt_tt_bh_emul_config *config =
		(const struct pvt_tt_bh_emul_config *)dev->config;
	enum pvt_tt_bh_attribute pvt_attr = (enum pvt_tt_bh_attribute)attr;

	switch (pvt_attr) {
	case SENSOR_ATTR_PVT_TT_BH_NUM_PD:
		val->val1 = config->num_pd;
		break;
	case SENSOR_ATTR_PVT_TT_BH_NUM_VM:
		val->val1 = config->num_vm;
		break;
	case SENSOR_ATTR_PVT_TT_BH_NUM_TS:
		val->val1 = config->num_ts;
		break;
	default:
		return -ENOTSUP;
	}

	val->val2 = 0;
	return 0;
}

static void pvt_tt_bh_emul_submit(const struct device *sensor, struct rtio_iodev_sqe *sqe)
{
	ARG_UNUSED(sensor);
	const struct rtio_sqe *event = &sqe->sqe;

	if (!event->iodev) {
		LOG_ERR("IO device is null");
		rtio_iodev_sqe_err(sqe, -EINVAL);
		return;
	}

	if (event->op != RTIO_OP_RX) {
		LOG_ERR("Sensor submit expects the RX opcode");
		rtio_iodev_sqe_err(sqe, -EINVAL);
		return;
	}

	const struct sensor_read_config *sensor_cfg =
		(const struct sensor_read_config *)event->iodev->data;
	const struct pvt_tt_bh_emul_config *cfg =
		(const struct pvt_tt_bh_emul_config *)sensor_cfg->sensor->config;
	uint32_t min_buffer_len = sizeof(struct pvt_tt_bh_rtio_data) * sensor_cfg->count;
	uint8_t *buf;
	uint32_t buf_len;
	int ret;

	ret = rtio_sqe_rx_buf(sqe, min_buffer_len, min_buffer_len, &buf, &buf_len);
	if (ret != 0) {
		LOG_ERR("Failed to get a read buffer of size %u bytes", min_buffer_len);
		rtio_iodev_sqe_err(sqe, ret);
		return;
	}

	struct pvt_tt_bh_rtio_data *data = (struct pvt_tt_bh_rtio_data *)buf;

	for (size_t i = 0; i < sensor_cfg->count; i++) {
		const struct sensor_chan_spec *chan = &sensor_cfg->channels[i];

		data[i].spec = *chan;

		switch (chan->chan_type) {
		case SENSOR_CHAN_PVT_TT_BH_PD:
			if (chan->chan_idx >= cfg->num_pd) {
				rtio_iodev_sqe_err(sqe, -EINVAL);
				return;
			}
			data[i].raw = cfg->pd_raw;
			break;
		case SENSOR_CHAN_PVT_TT_BH_VM:
			if (chan->chan_idx >= cfg->num_vm) {
				rtio_iodev_sqe_err(sqe, -EINVAL);
				return;
			}
			data[i].raw = cfg->vm_raw;
			break;
		case SENSOR_CHAN_PVT_TT_BH_TS:
			if (chan->chan_idx >= cfg->num_ts) {
				rtio_iodev_sqe_err(sqe, -EINVAL);
				return;
			}
			data[i].raw = cfg->ts_raw;
			break;
		case SENSOR_CHAN_PVT_TT_BH_TS_AVG:
			data[i].raw = cfg->ts_raw;
			break;
		default:
			LOG_ERR("Unsupported channel type: %d", chan->chan_type);
			rtio_iodev_sqe_err(sqe, -ENOTSUP);
			return;
		}
	}

	rtio_iodev_sqe_ok(sqe, 0);
}

static int pvt_tt_bh_emul_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

__weak void pvt_tt_bh_delay_chain_set(uint32_t new_delay_chain_)
{
	ARG_UNUSED(new_delay_chain_);
}

static DEVICE_API(sensor, pvt_tt_bh_emul_driver_api) = {
	.attr_set = NULL,
	.attr_get = pvt_tt_bh_emul_attr_get,
	.trigger_set = NULL,
	.sample_fetch = NULL,
	.channel_get = NULL,
	.submit = pvt_tt_bh_emul_submit,
	.get_decoder = pvt_tt_bh_get_decoder,
};

#define DEFINE_PVT_TT_BH_EMUL(id)                                                                  \
	static const struct pvt_tt_bh_emul_config pvt_tt_bh_emul_config_##id = {                   \
		.num_ts = DT_PROP(DT_DRV_INST(id), num_ts),                                        \
		.num_pd = DT_PROP(DT_DRV_INST(id), num_pd),                                        \
		.num_vm = DT_PROP(DT_DRV_INST(id), num_vm),                                        \
		.pd_raw = DT_PROP(DT_DRV_INST(id), pd_raw),                                        \
		.vm_raw = DT_PROP(DT_DRV_INST(id), vm_raw),                                        \
		.ts_raw = DT_PROP(DT_DRV_INST(id), ts_raw),                                        \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(id, pvt_tt_bh_emul_init, NULL, NULL, &pvt_tt_bh_emul_config_##id,    \
			      POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,                            \
			      &pvt_tt_bh_emul_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DEFINE_PVT_TT_BH_EMUL)
