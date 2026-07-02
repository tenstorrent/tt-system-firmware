/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "avs.h"
#include "gddr.h"
#include "telemetry_internal.h"
#include "regulator.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/tenstorrent/pvt_tt_bh.h>

static int64_t last_update_time;
static TelemetryInternalData internal_data;

static const struct device *const pvt = DEVICE_DT_GET(DT_NODELABEL(pvt));

SENSOR_DT_READ_IODEV(ts_avg_iodev, DT_NODELABEL(pvt), {SENSOR_CHAN_PVT_TT_BH_TS_AVG, 0});

RTIO_DEFINE(ts_avg_ctx, 1, 1);

static uint8_t ts_avg_buf[sizeof(struct pvt_tt_bh_rtio_data)];

/**
 * @brief Read telemetry values that are shared by multiple components
 *
 * This function will update the cached TelemetryInternalData values if necessary.
 * Then return a copy of the values through the *data pointer.
 *
 * @param max_staleness Maximum time interval in milliseconds since the last update
 * @param data Pointer to the TelemetryInternalData struct to fill with the values
 */
void ReadTelemetryInternal(int64_t max_staleness, TelemetryInternalData *data)
{
	int64_t reftime = last_update_time;

	if (k_uptime_delta(&reftime) >= max_staleness) {
		float avg_tmp;
		const struct sensor_decoder_api *decoder;

		sensor_get_decoder(pvt, &decoder);
		sensor_read(&ts_avg_iodev, &ts_avg_ctx, ts_avg_buf, sizeof(ts_avg_buf));

		decoder->decode(ts_avg_buf,
				(struct sensor_chan_spec){SENSOR_CHAN_PVT_TT_BH_TS_AVG, 0}, NULL, 1,
				&avg_tmp);

		internal_data.asic_temperature = avg_tmp;

		/* Get all dynamically updated values */
#ifndef CONFIG_TT_BH_ARC_EMUL
		internal_data.vcore_voltage = get_vcore();
		AVSReadCurrent(AVS_VCORE_RAIL, &internal_data.vcore_current);
		internal_data.vcore_power =
			internal_data.vcore_current * internal_data.vcore_voltage * 0.001f;
		internal_data.gddr_io_power_west = GetGddrWestIoPower();
		internal_data.gddr_io_power_east = GetGddrEastIoPower();

		(void)get_gddr_temps(&internal_data.gddr_temps);
#endif
		/* reftime was updated to the current uptime by the k_uptime_delta() call */
		last_update_time = reftime;
	}

	*data = internal_data;
}
