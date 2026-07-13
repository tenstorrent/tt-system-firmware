/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PVT_TT_BH_H
#define PVT_TT_BH_H

#include <zephyr/drivers/sensor.h>

enum pvt_tt_bh_attribute {
	SENSOR_ATTR_PVT_TT_BH_NUM_PD = SENSOR_ATTR_PRIV_START,
	SENSOR_ATTR_PVT_TT_BH_NUM_VM,
	SENSOR_ATTR_PVT_TT_BH_NUM_TS,
};

/**
 * Information for each device will be stored as a `struct sensor_chan_spec`,
 * which contains the channel (TS, VM or PD) and the index.
 */
enum pvt_tt_bh_channel {
	SENSOR_CHAN_PVT_TT_BH_PD = SENSOR_CHAN_PRIV_START,
	SENSOR_CHAN_PVT_TT_BH_VM,
	SENSOR_CHAN_PVT_TT_BH_TS,
	SENSOR_CHAN_PVT_TT_BH_TS_AVG,
};

typedef enum {
	ReadOk = 0,
	SampleFault = 1,
	IncorrectSampleType = 2,
	SdifTimeout = 3,
} ReadStatus;

struct pvt_tt_bh_config {
	uint8_t num_pd;
	uint8_t num_vm;
	uint8_t num_ts;

	/*
	 * Single-point calibration delta values for each temperature sensor.
	 *
	 * During device instantiation (DEFINE_PVT_TT_BH macro), a static array
	 * of size num_ts is allocated and zero-initialized for each device instance.
	 * The pointer is then set to reference this static array.
	 *
	 * During pvt_tt_bh_init(), each sensor's 25C calibration value is read from
	 * the functional eFuse. The delta between this eFuse value and the expected
	 * raw value for 25C is calculated and stored here.
	 *
	 * To apply calibration: calibrated_reading = raw_reading - therm_cali_delta[sensor_id]
	 *
	 * Values are in raw sensor units (not celcius). Positive delta means the
	 * sensor reads higher than expected, negative means it reads lower.
	 *
	 * Only populated if the eFuse calibration value is within 3C of 25C
	 * (22.0C to 28.0C range). If outside this range, the delta remains 0
	 * (no calibration applied).
	 *
	 * Array allocation: Static array created per device instance via DEFINE_PVT_TT_BH
	 * Array size: num_ts elements (from devicetree property)
	 * Initialization: Zero-filled during compilation, populated during pvt_tt_bh_init()
	 */
	int16_t *therm_cali_delta;
};

struct pvt_tt_bh_data {
};

/*
 * Raw sensor data that will be submitted to the rtio buffer for the decoder
 * to then use.
 */
struct pvt_tt_bh_rtio_data {
	struct sensor_chan_spec spec;
	uint16_t raw; /* Raw sensor data can fit within 16 bits. */
};

/*
 * Convert raw temperature sensor data to celcius.
 */
float pvt_tt_bh_raw_to_temp(uint16_t raw);

/*
 * Convert celcius into raw temperature sensor data.
 */
uint16_t pvt_tt_bh_temp_to_raw(float value);

/*
 * Convert raw voltage monitor data to volts.
 */
float pvt_tt_bh_raw_to_volt(uint16_t raw);

/*
 * Convert voltage insto raw voltage monitor data;
 */
uint16_t pvt_tt_bh_volt_to_raw(float value);

/*
 * Convert raw process detector data to MHz.
 */
float pvt_tt_bh_raw_to_freq(uint16_t raw);

/*
 * Convert frequency into raw process detector data.
 */
uint16_t pvt_tt_bh_freq_to_raw(float value);

int pvt_tt_bh_get_decoder(const struct device *dev, const struct sensor_decoder_api **api);

void pvt_tt_bh_submit(const struct device *sensor, struct rtio_iodev_sqe *sqe);

void pvt_tt_bh_delay_chain_set(uint32_t new_delay_chain_);

/* Test/emulation hook: update the emulated TS raw sample for PVT sensor instance 0. */
#ifdef CONFIG_PVT_TT_BH_EMUL
int pvt_tt_bh_emul_set_ts_raw(uint16_t raw);
#endif /* CONFIG_PVT_TT_BH_EMUL */

#endif /* PVT_TT_BH_H */
