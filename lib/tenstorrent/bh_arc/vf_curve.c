/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/util.h>
#include "aiclk_ppm.h"
#include "vf_curve.h"
#include <zephyr/drivers/misc/bh_fwtable.h>
#include <tenstorrent/msgqueue.h>
#include <tenstorrent/smc_msg.h>
#include "functional_efuse.h"

/* Bounds checks for frequency and voltage margin */
#define FREQ_MARGIN_MAX    300.0F
#define FREQ_MARGIN_MIN    -300.0F
#define VOLTAGE_MARGIN_MAX 150.0F
#define VOLTAGE_MARGIN_MIN -150.0F

/* Process-based VF curve coefficients */
#define VF_INTERCEPT     738.687863F
#define VF_COEFF_RO      -9.399314F
#define VF_COEFF_FREQ    52.359951F
#define VF_COEFF_RO_SQ   -4.295651F
#define VF_COEFF_RO_FREQ 0.000000F
#define VF_COEFF_FREQ_SQ 8.560169F

/* Normalization parameters */
#define RO_NORM_MEAN   2582.380952F
#define RO_NORM_STD    121.320464F
#define FREQ_NORM_MEAN 1279.761905F
#define FREQ_NORM_STD  189.567027F

/* Process-based margin thresholds and values */
#define RO_SS_THRESHOLD        2500.0F
#define FREQ_THRESHOLD_MHZ     1200.0F
#define SS_MARGIN_MV           20.0F
#define FF_LOW_FREQ_MARGIN_MV  0.0F
#define FF_HIGH_FREQ_MARGIN_MV 20.0F

/* Legacy VF curve coefficients (fallback for missing efuse) */
#define VF_QUADRATIC_COEFF 0.00031395F
#define VF_LINEAR_COEFF    -0.43953F
#define VF_CONSTANT        828.83F

static float freq_margin_mhz = FREQ_MARGIN_MAX;
static float fw_table_voltage_margin_mv = VOLTAGE_MARGIN_MAX;
static uint32_t process_RO;
static bool use_process_vf_curve;
static bool process_is_ss;

/* Precomputed from process_RO during init:
 *   vf_ro_base      = VF_INTERCEPT + VF_COEFF_RO * ro_n + VF_COEFF_RO_SQ * ro_n^2
 *   vf_freq_linear  = VF_COEFF_FREQ + VF_COEFF_RO_FREQ * ro_n
 */
static float vf_ro_base;
static float vf_freq_linear;

static const struct device *const fwtable_dev = DEVICE_DT_GET(DT_NODELABEL(fwtable));

void InitVFCurve(void)
{
	process_RO = READ_FUNCTIONAL_EFUSE(PROCESS_RO);

	use_process_vf_curve = (process_RO != 0);
	process_is_ss = process_RO < RO_SS_THRESHOLD;

	if (use_process_vf_curve) {
		float ro_n = (process_RO - RO_NORM_MEAN) / RO_NORM_STD;

		vf_ro_base = VF_INTERCEPT + VF_COEFF_RO * ro_n + VF_COEFF_RO_SQ * ro_n * ro_n;
		vf_freq_linear = VF_COEFF_FREQ + VF_COEFF_RO_FREQ * ro_n;
	}

	freq_margin_mhz =
		CLAMP(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.frequency_margin,
		      FREQ_MARGIN_MIN, FREQ_MARGIN_MAX);
	fw_table_voltage_margin_mv =
		CLAMP(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.voltage_margin,
		      VOLTAGE_MARGIN_MIN, VOLTAGE_MARGIN_MAX);
}

/**
 * @brief Calculate the voltage based on the frequency
 *
 * @param freq_mhz The frequency in MHz
 * @return The voltage in mV
 */
float VFCurve(float freq_mhz)
{
	if (!use_process_vf_curve) {
		float freq_with_margin_mhz = freq_mhz + freq_margin_mhz;
		float voltage_mv =
			VF_QUADRATIC_COEFF * freq_with_margin_mhz * freq_with_margin_mhz +
			VF_LINEAR_COEFF * freq_with_margin_mhz + VF_CONSTANT;

		return voltage_mv + fw_table_voltage_margin_mv;
	}

	float voltage_margin;

	/* Process-based margin is FW-table base voltage margin plus process/frequency delta. */
	if (process_is_ss) {
		voltage_margin = fw_table_voltage_margin_mv + SS_MARGIN_MV;
	} else {
		voltage_margin = (freq_mhz >= FREQ_THRESHOLD_MHZ)
					 ? fw_table_voltage_margin_mv + FF_HIGH_FREQ_MARGIN_MV
					 : fw_table_voltage_margin_mv + FF_LOW_FREQ_MARGIN_MV;
	}

	float freq_n = (freq_mhz - FREQ_NORM_MEAN) / FREQ_NORM_STD;

	float voltage_mv =
		vf_ro_base + vf_freq_linear * freq_n + VF_COEFF_FREQ_SQ * freq_n * freq_n;

	return voltage_mv + voltage_margin;
}

static uint8_t get_voltage_curve_from_freq_handler(const union request *request,
						   struct response *response)
{
	float input_freq_mhz = (float)request->get_voltage_curve_from_freq.input_freq_mhz;
	float voltage_mv = VFCurve(input_freq_mhz);

	if (voltage_mv < 0.0F) {
		response->data[1] = 0U;
	} else {
		response->data[1] = (uint32_t)(voltage_mv);
	}

	return 0;
}

static uint8_t get_freq_curve_from_voltage_handler(const union request *request,
						   struct response *response)
{
	int input_voltage_mv = request->get_freq_curve_from_voltage.input_voltage_mv;
	int freq_mhz = GetMaxAiclkForVoltage(input_voltage_mv);

	response->data[1] = freq_mhz;

	return 0;
}

REGISTER_MESSAGE(TT_SMC_MSG_GET_VOLTAGE_CURVE_FROM_FREQ, get_voltage_curve_from_freq_handler);
REGISTER_MESSAGE(TT_SMC_MSG_GET_FREQ_CURVE_FROM_VOLTAGE, get_freq_curve_from_voltage_handler);
