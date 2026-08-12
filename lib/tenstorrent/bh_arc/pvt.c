/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <tenstorrent/reg.h>
#include "telemetry.h"
#include "timer.h"

#include <float.h> /* for FLT_MAX */

#include <tenstorrent/smc_msg.h>
#include <tenstorrent/msgqueue.h>
#include <tenstorrent/post_code.h>
#include <tenstorrent/sys_init_defines.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/tenstorrent/pvt_tt_bh.h>

static const struct device *const pvt = DEVICE_DT_GET(DT_NODELABEL(pvt));

SENSOR_DT_READ_IODEV(vm_iodev, DT_NODELABEL(pvt), {SENSOR_CHAN_PVT_TT_BH_VM, 0},
		     {SENSOR_CHAN_PVT_TT_BH_VM, 1}, {SENSOR_CHAN_PVT_TT_BH_VM, 2},
		     {SENSOR_CHAN_PVT_TT_BH_VM, 3}, {SENSOR_CHAN_PVT_TT_BH_VM, 4},
		     {SENSOR_CHAN_PVT_TT_BH_VM, 5}, {SENSOR_CHAN_PVT_TT_BH_VM, 6},
		     {SENSOR_CHAN_PVT_TT_BH_VM, 7});

SENSOR_DT_READ_IODEV(ts_iodev, DT_NODELABEL(pvt), {SENSOR_CHAN_PVT_TT_BH_TS, 0},
		     {SENSOR_CHAN_PVT_TT_BH_TS, 1}, {SENSOR_CHAN_PVT_TT_BH_TS, 2},
		     {SENSOR_CHAN_PVT_TT_BH_TS, 3}, {SENSOR_CHAN_PVT_TT_BH_TS, 4},
		     {SENSOR_CHAN_PVT_TT_BH_TS, 5}, {SENSOR_CHAN_PVT_TT_BH_TS, 6},
		     {SENSOR_CHAN_PVT_TT_BH_TS, 7});

SENSOR_DT_READ_IODEV(pd_iodev, DT_NODELABEL(pvt), {SENSOR_CHAN_PVT_TT_BH_PD, 0},
		     {SENSOR_CHAN_PVT_TT_BH_PD, 1}, {SENSOR_CHAN_PVT_TT_BH_PD, 2},
		     {SENSOR_CHAN_PVT_TT_BH_PD, 3}, {SENSOR_CHAN_PVT_TT_BH_PD, 4},
		     {SENSOR_CHAN_PVT_TT_BH_PD, 5}, {SENSOR_CHAN_PVT_TT_BH_PD, 6},
		     {SENSOR_CHAN_PVT_TT_BH_PD, 7}, {SENSOR_CHAN_PVT_TT_BH_PD, 8},
		     {SENSOR_CHAN_PVT_TT_BH_PD, 9}, {SENSOR_CHAN_PVT_TT_BH_PD, 10},
		     {SENSOR_CHAN_PVT_TT_BH_PD, 11}, {SENSOR_CHAN_PVT_TT_BH_PD, 12},
		     {SENSOR_CHAN_PVT_TT_BH_PD, 13}, {SENSOR_CHAN_PVT_TT_BH_PD, 14},
		     {SENSOR_CHAN_PVT_TT_BH_PD, 15});

RTIO_DEFINE(pvt_ctx, 16, 16);

/*
 * Used for storing raw sensor data when reading.
 */
static struct pvt_tt_bh_rtio_data pd_buf[DT_PROP(DT_NODELABEL(pvt), num_pd)];
static struct pvt_tt_bh_rtio_data vm_buf[DT_PROP(DT_NODELABEL(pvt), num_vm)];
static struct pvt_tt_bh_rtio_data ts_buf[DT_PROP(DT_NODELABEL(pvt), num_ts)];

/* return selected TS raw reading and temperature in telemetry format */
static uint8_t read_ts_handler(const union request *request, struct response *response)
{
	float celcius;
	const struct sensor_decoder_api *decoder;
	const struct pvt_tt_bh_config *pvt_cfg = pvt->config;
	int ret;

	ret = sensor_get_decoder(pvt, &decoder);
	if (ret != 0) {
		return ret;
	}

	ret = sensor_read(&ts_iodev, &pvt_ctx, (uint8_t *)ts_buf, sizeof(ts_buf));
	if (ret != 0) {
		return ret;
	}
	uint32_t id = request->read_ts.sensor_id;

	decoder->decode((uint8_t *)ts_buf, (struct sensor_chan_spec){SENSOR_CHAN_PVT_TT_BH_TS, id},
			NULL, pvt_cfg->num_ts, &celcius);

	response->data[1] = pvt_tt_bh_temp_to_raw(celcius);
	response->data[2] = ConvertFloatToTelemetry(celcius);

	return ret;
}

/* return selected PD raw reading and frequency in telemetry format */
static uint8_t read_pd_handler(const union request *request, struct response *response)
{
	float freq;
	const struct sensor_decoder_api *decoder;
	const struct pvt_tt_bh_config *pvt_cfg = pvt->config;
	int ret;

	uint32_t delay_chain = request->read_pd.delay_chain;

	pvt_tt_bh_delay_chain_set(delay_chain);

	ret = sensor_get_decoder(pvt, &decoder);
	if (ret != 0) {
		return ret;
	}

	ret = sensor_read(&pd_iodev, &pvt_ctx, (uint8_t *)pd_buf, sizeof(pd_buf));
	if (ret != 0) {
		return ret;
	}
	uint32_t id = request->read_pd.sensor_id;

	decoder->decode((uint8_t *)pd_buf, (struct sensor_chan_spec){SENSOR_CHAN_PVT_TT_BH_PD, id},
			NULL, pvt_cfg->num_pd, &freq);

	response->data[1] = pvt_tt_bh_freq_to_raw(freq);
	response->data[2] = ConvertFloatToTelemetry(freq);

	return ret;
}

/* return selected VM raw reading and voltage in mV */
static uint8_t read_vm_handler(const union request *request, struct response *response)
{
	float volts;
	const struct sensor_decoder_api *decoder;
	const struct pvt_tt_bh_config *pvt_cfg = pvt->config;
	int ret;

	ret = sensor_get_decoder(pvt, &decoder);
	if (ret != 0) {
		return ret;
	}

	ret = sensor_read(&vm_iodev, &pvt_ctx, (uint8_t *)vm_buf, sizeof(vm_buf));
	if (ret != 0) {
		return ret;
	}

	uint32_t id = request->read_vm.sensor_id;

	decoder->decode((uint8_t *)vm_buf, (struct sensor_chan_spec){SENSOR_CHAN_PVT_TT_BH_VM, id},
			NULL, pvt_cfg->num_vm, &volts);

	response->data[1] = pvt_tt_bh_volt_to_raw(volts);
	response->data[2] = (uint16_t)(volts * 1000);

	return ret;
}

REGISTER_MESSAGE(TT_SMC_MSG_READ_TS, read_ts_handler);
REGISTER_MESSAGE(TT_SMC_MSG_READ_PD, read_pd_handler);
REGISTER_MESSAGE(TT_SMC_MSG_READ_VM, read_vm_handler);
