/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_bh_arc_telemetry

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/smbus.h>
#include <zephyr/logging/log.h>
#include <zephyr/rtio/rtio.h>
#include <zephyr/rtio/work.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <tenstorrent/tt_smbus_regs.h>

#include <errno.h>

#include <telemetry.h>

LOG_MODULE_REGISTER(bh_arc_telem, CONFIG_SENSOR_LOG_LEVEL);

/* Max bytes returned by a single-tag CMFW_SMBUS_TELEMETRY_READ (56-bit payload) */
#define BH_ARC_TELEM_RCV_BUF_LEN 7U

struct bh_arc_telem_config {
	struct smbus_dt_spec smbus;
	const struct bh_arc_telem_map *maps;
	size_t map_count;
};

struct bh_arc_telem_map {
	enum sensor_channel channel;
	uint8_t channel_index;
	uint8_t telem_tag;
	int32_t scale_micro;
};

struct bh_arc_telem_rtio_data {
	struct sensor_chan_spec spec;
	uint8_t telem_tag;
	int32_t scale_micro;
	uint32_t raw;
};

static int bh_arc_telem_raw_to_sensor_value(uint8_t telem_tag, int32_t scale_micro, uint32_t raw,
					    struct sensor_value *val)
{
	int32_t signed_raw;
	int64_t scaled;

	if (val == NULL) {
		return -EINVAL;
	}

	if (telem_tag == TAG_ASIC_TEMPERATURE) {
		signed_raw = (int32_t)raw;
		val->val1 = signed_raw / 65536;
		val->val2 = (int32_t)(((int64_t)(signed_raw % 65536) * 1000000LL) / 65536LL);
		return 0;
	}

	scaled = (int64_t)(int32_t)raw * scale_micro;
	val->val1 = (int32_t)(scaled / 1000000LL);
	val->val2 = (int32_t)(scaled % 1000000LL);

	return 0;
}

static const struct bh_arc_telem_map *bh_arc_telem_find_map(const struct bh_arc_telem_config *cfg,
							    enum sensor_channel channel,
							    uint8_t channel_index)
{
	for (size_t i = 0; i < cfg->map_count; i++) {
		if (cfg->maps[i].channel == channel &&
		    cfg->maps[i].channel_index == channel_index) {
			return &cfg->maps[i];
		}
	}

	return NULL;
}

static int bh_arc_telem_read_raw(const struct bh_arc_telem_config *cfg,
				 const struct bh_arc_telem_map *map, uint32_t *raw)
{
	uint8_t snd_buf[1];
	uint8_t rcv_buf[BH_ARC_TELEM_RCV_BUF_LEN];
	uint8_t rcv_count = 0U;
	int rc;

	if (raw == NULL) {
		return -EINVAL;
	}

	if ((map->telem_tag == 0U) || (map->telem_tag >= TAG_COUNT)) {
		LOG_ERR("Invalid telemetry tag: %u", map->telem_tag);
		return -EINVAL;
	}

	snd_buf[0] = map->telem_tag;

	rc = smbus_byte_data_write(cfg->smbus.bus, cfg->smbus.addr, CMFW_SMBUS_TELEMETRY_READ_CRC,
				   snd_buf[0]);
	if (rc != 0) {
		return rc;
	}

	rc = smbus_block_read(cfg->smbus.bus, cfg->smbus.addr, CMFW_SMBUS_TELEMETRY_READ_CRC_DATA,
			      &rcv_count, rcv_buf);
	if (rc != 0) {
		return rc;
	}

	if (rcv_count < 7U) {
		LOG_ERR("Short telemetry response: %u bytes", rcv_count);
		return -EIO;
	}

	/* Telemetry data is at bytes [3:6], skip first 3 status bytes */
	*raw = sys_get_le32(&rcv_buf[3]);
	return 0;
}

static int bh_arc_telem_decoder_get_frame_count(const uint8_t *buffer,
						struct sensor_chan_spec chan_spec,
						uint16_t *frame_count)
{
	const struct bh_arc_telem_rtio_data *sample = (const struct bh_arc_telem_rtio_data *)buffer;

	if (buffer == NULL || frame_count == NULL) {
		return -EINVAL;
	}

	*frame_count = sensor_chan_spec_eq(sample->spec, chan_spec) ? 1U : 0U;
	return 0;
}

static int bh_arc_telem_decoder_get_size_info(struct sensor_chan_spec channel, size_t *base_size,
					      size_t *frame_size)
{
	ARG_UNUSED(channel);

	if (base_size == NULL || frame_size == NULL) {
		return -EINVAL;
	}

	*base_size = sizeof(struct bh_arc_telem_rtio_data);
	*frame_size = sizeof(struct bh_arc_telem_rtio_data);
	return 0;
}

static int bh_arc_telem_decoder_decode(const uint8_t *buffer, struct sensor_chan_spec chan_spec,
				       uint32_t *fit, uint16_t max_count, void *data_out)
{
	struct sensor_value *out = data_out;
	const struct bh_arc_telem_rtio_data *samples =
		(const struct bh_arc_telem_rtio_data *)buffer;
	int ret;

	if (buffer == NULL || fit == NULL || data_out == NULL || max_count == 0U) {
		return -EINVAL;
	}

	if (*fit >= max_count) {
		return -ENODATA;
	}

	for (uint32_t i = *fit; i < max_count; i++) {
		if (!sensor_chan_spec_eq(samples[i].spec, chan_spec)) {
			continue;
		}

		ret = bh_arc_telem_raw_to_sensor_value(samples[i].telem_tag, samples[i].scale_micro,
						       samples[i].raw, out);
		if (ret != 0) {
			return ret;
		}

		*fit = i + 1U;
		return 1;
	}

	return -ENODATA;
}

static bool bh_arc_telem_decoder_has_trigger(const uint8_t *buffer, enum sensor_trigger_type trig)
{
	ARG_UNUSED(buffer);
	ARG_UNUSED(trig);

	return false;
}

SENSOR_DECODER_API_DT_DEFINE() = {
	.get_frame_count = bh_arc_telem_decoder_get_frame_count,
	.get_size_info = bh_arc_telem_decoder_get_size_info,
	.decode = bh_arc_telem_decoder_decode,
	.has_trigger = bh_arc_telem_decoder_has_trigger,
};

static int bh_arc_telem_get_decoder(const struct device *dev,
				    const struct sensor_decoder_api **decoder)
{
	ARG_UNUSED(dev);

	*decoder = &SENSOR_DECODER_NAME();
	return 0;
}

static void bh_arc_telem_submit_sample(struct rtio_iodev_sqe *iodev_sqe)
{
	const struct sensor_read_config *sensor_cfg =
		(const struct sensor_read_config *)iodev_sqe->sqe.iodev->data;
	const struct bh_arc_telem_config *cfg = sensor_cfg->sensor->config;
	uint32_t min_buffer_len = sizeof(struct bh_arc_telem_rtio_data) * sensor_cfg->count;
	uint8_t *buf;
	uint32_t buf_len;
	uint32_t raw;
	int ret;

	ret = rtio_sqe_rx_buf(iodev_sqe, min_buffer_len, min_buffer_len, &buf, &buf_len);
	if (ret != 0) {
		LOG_ERR("Failed to get read buffer of %u bytes", min_buffer_len);
		rtio_iodev_sqe_err(iodev_sqe, ret);
		return;
	}

	if (buf_len < min_buffer_len) {
		rtio_iodev_sqe_err(iodev_sqe, -ENOMEM);
		return;
	}

	struct bh_arc_telem_rtio_data *data = (struct bh_arc_telem_rtio_data *)buf;

	for (size_t i = 0; i < sensor_cfg->count; i++) {
		const struct sensor_chan_spec *chan = &sensor_cfg->channels[i];
		const struct bh_arc_telem_map *map = bh_arc_telem_find_map(
			cfg, (enum sensor_channel)chan->chan_type, (uint8_t)chan->chan_idx);

		if (map == NULL) {
			rtio_iodev_sqe_err(iodev_sqe, -ENOTSUP);
			return;
		}

		ret = bh_arc_telem_read_raw(cfg, map, &raw);
		if (ret != 0) {
			rtio_iodev_sqe_err(iodev_sqe, ret);
			return;
		}

		data[i].spec = *chan;
		data[i].telem_tag = map->telem_tag;
		data[i].scale_micro = map->scale_micro;
		data[i].raw = raw;
	}

	rtio_iodev_sqe_ok(iodev_sqe, 0);
}

static void bh_arc_telem_submit(const struct device *sensor, struct rtio_iodev_sqe *sqe)
{
	const struct rtio_sqe *event = &sqe->sqe;

	if (!event->iodev) {
		rtio_iodev_sqe_err(sqe, -EINVAL);
		return;
	}

	if (event->op != RTIO_OP_RX) {
		rtio_iodev_sqe_err(sqe, -EINVAL);
		return;
	}

	ARG_UNUSED(sensor);

	struct rtio_work_req *req = rtio_work_req_alloc();

	if (req == NULL) {
		rtio_iodev_sqe_err(sqe, -ENOMEM);
		return;
	}

	rtio_work_req_submit(req, sqe, bh_arc_telem_submit_sample);
}

static int bh_arc_telem_init(const struct device *dev)
{
	const struct bh_arc_telem_config *cfg = dev->config;

	if (!device_is_ready(cfg->smbus.bus)) {
		LOG_ERR("%s: SMBus bus %s not ready", dev->name, cfg->smbus.bus->name);
		return -ENODEV;
	}

	if (cfg->map_count == 0U) {
		LOG_ERR("%s: no channel mappings configured", dev->name);
		return -EINVAL;
	}

	return 0;
}

static DEVICE_API(sensor, bh_arc_telem_api) = {
	.sample_fetch = NULL,
	.channel_get = NULL,
	.submit = bh_arc_telem_submit,
	.get_decoder = bh_arc_telem_get_decoder,
};

#define BH_ARC_TELEM_MAP_ENTRY(node_id)                                                            \
	{                                                                                          \
		.channel = (enum sensor_channel)DT_PROP(node_id, channel_type),                    \
		.channel_index = DT_PROP(node_id, channel_index),                                  \
		.telem_tag = DT_PROP(node_id, telemetry_tag),                                      \
		.scale_micro = DT_PROP(node_id, scale_micro),                                      \
	},

#define BH_ARC_TELEM_MAP_COUNT(inst) DT_CHILD_NUM_STATUS_OKAY(DT_DRV_INST(inst))

#define BH_ARC_TELEM_DEFINE(inst)                                                                  \
	static const struct bh_arc_telem_map                                                       \
		bh_arc_telem_maps_##inst[MAX(1, BH_ARC_TELEM_MAP_COUNT(inst))] = {                 \
			DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), BH_ARC_TELEM_MAP_ENTRY)};  \
	static const struct bh_arc_telem_config bh_arc_telem_cfg_##inst = {                        \
		.smbus = SMBUS_DT_SPEC_GET(DT_PHANDLE(DT_DRV_INST(inst), arc)),                    \
		.maps = bh_arc_telem_maps_##inst,                                                  \
		.map_count = BH_ARC_TELEM_MAP_COUNT(inst),                                         \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, bh_arc_telem_init, NULL, NULL, &bh_arc_telem_cfg_##inst,       \
			      POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY, &bh_arc_telem_api)

DT_INST_FOREACH_STATUS_OKAY(BH_ARC_TELEM_DEFINE)
