/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/zbus/zbus.h>
#include "throttler.h"
#include "aiclk_ppm.h"
#include <tenstorrent/smc_msg.h>
#include <tenstorrent/msgqueue.h>
#include "cm2dm_msg.h"
#include <zephyr/drivers/misc/bh_fwtable.h>
#include "telemetry_internal.h"
#include "telemetry.h"
#include "noc2axi.h"
#include "tensix_state_msg.h"

static uint32_t power_limit;

static bool doppler;
static bool doppler_slow;
static bool doppler_t2;
static bool doppler_t3;
static const bool thermal_throttling = true;

#define kThrottlerAiclkScaleFactor 500.0F
#define DEFAULT_BOARD_POWER_LIMIT  150

LOG_MODULE_REGISTER(throttler);

static const struct device *const fwtable_dev = DEVICE_DT_GET(DT_NODELABEL(fwtable));

typedef enum {
	kThrottlerTDP,
	kThrottlerFastTDC,
	kThrottlerTDC,
	kThrottlerThm,
	kThrottlerBoardPower,
	kThrottlerGDDRThm,
	kThrottlerDopplerSlow,
	kThrottlerCount,
} ThrottlerId;

typedef struct {
	float min;
	float max;
} ThrottlerLimitRange;

/* This table is used to restrict the throttler limits to reasonable ranges. */
/* They are passed in from the FW table in SPI */
/* clang-format off */
static const ThrottlerLimitRange throttler_limit_ranges[kThrottlerCount] = {
	[kThrottlerTDP]		= { .min = 50, .max = 500, },
	[kThrottlerFastTDC]	= { .min = 50, .max = 500, },
	[kThrottlerTDC]		= { .min = 50, .max = 400, },
	[kThrottlerThm]		= { .min = 50, .max = 100, },
	[kThrottlerBoardPower]	= { .min = 50, .max = 600, },
	[kThrottlerGDDRThm]	= { .min = 50, .max = 100, },
	[kThrottlerDopplerSlow]	= { .min = 50, .max = 1200, },
};
/* clang-format on */

typedef struct {
	float alpha_filter;
	float p_gain;
	float d_gain;
} ThrottlerParams;

typedef struct {
	const enum aiclk_arb_max arb_max; /* The arbiter associated with this throttler */

	const ThrottlerParams params;
	float limit;
	float value;
	float error;
	float prev_error;
	float output;
} Throttler;

/* clang-format off */
static Throttler throttler[kThrottlerCount] = {
	[kThrottlerTDP] = {
			.arb_max = aiclk_arb_max_tdp,
			.params = {
					.alpha_filter = 1.0,
					.p_gain = 0.015,
					.d_gain = 0.1,
				},
		},
	[kThrottlerFastTDC] = {
			.arb_max = aiclk_arb_max_fast_tdc,
			.params = {
					.alpha_filter = 1.0,
					.p_gain = 0.5,
					.d_gain = 0,
				},
		},
	[kThrottlerTDC] = {
			.arb_max = aiclk_arb_max_tdc,
			.params = {
					.alpha_filter = 0.1,
					.p_gain = 0.2,
					.d_gain = 0,
				},
		},
	[kThrottlerThm] = {
			.arb_max = aiclk_arb_max_thm,
			.params = {
					.alpha_filter = 1.0,
					.p_gain = 0.2,
					.d_gain = 0,
				},
		},
	[kThrottlerBoardPower] = {
			.arb_max = aiclk_arb_max_board_power,
			.params = {
					.alpha_filter = 1.0,
					.p_gain = 0.1,
					.d_gain = 0.1,
				},
		},
	[kThrottlerGDDRThm] = {
			.arb_max = aiclk_arb_max_gddr_thm,
			.params = {
					.alpha_filter = 1.0,
					.p_gain = 0.2,
					.d_gain = 0,
				},
		},
	[kThrottlerDopplerSlow] = {
			.arb_max = aiclk_arb_max_doppler_slow,
			.params = {
					.alpha_filter = 1.0,
					.p_gain = 0.0025,
					.d_gain = 0.3,
				},
		},
};
/* clang-format on */

static float get_throttler_clamped_limit(ThrottlerId id, float limit)
{
	return CLAMP(limit, throttler_limit_ranges[id].min, throttler_limit_ranges[id].max);
}

static void SetThrottlerLimit(ThrottlerId id, float limit)
{
	float clamped_limit = get_throttler_clamped_limit(id, limit);

	LOG_INF("Throttler %d limit set to %d", id, (uint32_t)clamped_limit);
	throttler[id].limit = clamped_limit;
}

static uint32_t throttle_counter;
static const uint32_t kKernelThrottleAddress = 0x10;
static bool tensixes_enabled = true;

static void BroadcastKernelThrottleState(void)
{
	const uint8_t kNocRing = 0;
	const uint8_t kNocTlb = 1;

	if (tensixes_enabled) {
		NOC2AXITensixBroadcastTlbSetup(kNocRing, kNocTlb, kKernelThrottleAddress,
					       kNoc2AxiOrderingStrict);
		NOC2AXIWrite32(kNocRing, kNocTlb, kKernelThrottleAddress, throttle_counter);
	}
}

static void InitKernelThrottling(void)
{
	throttle_counter = 0;

	BroadcastKernelThrottleState();
}

/* must only be called when throttle state changes */
static void SendKernelThrottlingMessage(bool throttle)
{
	/* The LLK uses fast = even, slow = odd, but for debug purposes, they'd like to
	 * know how many times throttling has happened. Just in case CMFW somehow gets
	 * out of sync internally, double-check the parity.
	 */
	throttle_counter++;
	if ((throttle_counter & 1) != throttle) {
		throttle_counter++;
	}

	BroadcastKernelThrottleState();
}

static void doppler_tensix_state_callback(const struct zbus_channel *chan)
{
	const struct tensix_state_msg *msg = zbus_chan_const_msg(chan);

	tensixes_enabled = msg->enable;

	BroadcastKernelThrottleState();
}

ZBUS_LISTENER_DEFINE(doppler_tensix_state_listener, doppler_tensix_state_callback);
ZBUS_CHAN_ADD_OBS(tensix_state_chan, doppler_tensix_state_listener, 0);

void InitThrottlers(void)
{
	doppler = tt_bh_fwtable_get_fw_table(fwtable_dev)->feature_enable.doppler_en;
	doppler_slow = doppler;
	doppler_t2 = doppler;
	doppler_t3 = doppler;

	SetThrottlerLimit(kThrottlerTDP,
			  tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit);
	SetThrottlerLimit(kThrottlerFastTDC,
			  tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdc_fast_limit);
	SetThrottlerLimit(kThrottlerTDC,
			  tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdc_limit);
	SetThrottlerLimit(kThrottlerThm,
			  tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.thm_limit);
	SetThrottlerLimit(kThrottlerBoardPower, DEFAULT_BOARD_POWER_LIMIT);
	SetThrottlerLimit(kThrottlerGDDRThm,
			  tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.gddr_thm_limit);

	SetThrottlerLimit(kThrottlerDopplerSlow, DEFAULT_BOARD_POWER_LIMIT);

	InitKernelThrottling();

	EnableArbMax(throttler[kThrottlerTDP].arb_max, !doppler);
	EnableArbMax(throttler[kThrottlerFastTDC].arb_max, !doppler);
	EnableArbMax(throttler[kThrottlerTDC].arb_max, !doppler);
	EnableArbMax(throttler[kThrottlerBoardPower].arb_max, !doppler);

	EnableArbMax(throttler[kThrottlerThm].arb_max, thermal_throttling);
	EnableArbMax(throttler[kThrottlerGDDRThm].arb_max, thermal_throttling);

	EnableArbMax(throttler[kThrottlerDopplerSlow].arb_max, doppler_slow);

	SetAiclkArbMax(aiclk_arb_max_doppler_critical, GetAiclkFmin());
	EnableArbMax(aiclk_arb_max_doppler_critical, false); /* enabled when limit triggered */
}

static void UpdateThrottler(ThrottlerId id, float value)
{
	Throttler *t = &throttler[id];

	t->value = t->params.alpha_filter * value + (1 - t->params.alpha_filter) * t->value;
	t->error = (t->limit - t->value) / t->limit;
	t->output = t->params.p_gain * t->error + t->params.d_gain * (t->error - t->prev_error);
	t->prev_error = t->error;
}

static void UpdateThrottlerArb(ThrottlerId id)
{
	Throttler *t = &throttler[id];

	float arb_val = GetThrottlerArbMax(t->arb_max);

	arb_val += t->output * kThrottlerAiclkScaleFactor;

	SetAiclkArbMax(t->arb_max, arb_val);
}

static uint16_t board_power_history[1000];
static uint16_t *board_power_history_cursor = board_power_history;
static uint32_t board_power_sum;
static bool kernel_nops_enabled;

static uint8_t t2_count;
static uint8_t t3_count;

#define ADVANCE_CIRCULAR_POINTER(pointer, array)                                                   \
	do {                                                                                       \
		if (++(pointer) == (array) + ARRAY_SIZE(array))                                    \
			(pointer) = (array);                                                       \
	} while (false)

static uint16_t UpdateMovingAveragePower(uint16_t current_power)
{
	board_power_sum += current_power - *board_power_history_cursor;
	*board_power_history_cursor = current_power;

	ADVANCE_CIRCULAR_POINTER(board_power_history_cursor, board_power_history);

	return board_power_sum / ARRAY_SIZE(board_power_history);
}

static bool DopplerActive(void)
{
	return doppler && power_limit > 0;
}

static void UpdateDoppler(const TelemetryInternalData *telemetry)
{
	uint16_t current_power = GetInputPower();
	uint16_t average_power = UpdateMovingAveragePower(current_power);

	UpdateThrottler(kThrottlerDopplerSlow, average_power);

	/* Doppler T2 throttler: 2x power limit for 10 consecutive samples */
	uint32_t t2_power_limit = power_limit * 2;

	if (current_power > t2_power_limit) {
		if (t2_count < UINT8_MAX) {
			t2_count++;
		}
	} else {
		t2_count = 0;
	}

	bool t2_triggered = t2_count >= 10 && doppler_t2;

	/* Doppler T3 throttler: 2.5x power limit for 2 consecutive samples */
	uint32_t t3_power_limit = power_limit * 5 / 2;

	if (current_power > t3_power_limit) {
		if (t3_count < UINT8_MAX) {
			t3_count++;
		}
	} else {
		t3_count = 0;
	}

	bool t3_triggered = t3_count >= 2 && doppler_t3;

	/* AICLK=Fmin isn't always enough to get below the board power limit. */
	bool start_nops = GetAiclkTarg() == GetAiclkFmin() && current_power > power_limit;
	bool stop_nops = GetAiclkTarg() == GetAiclkFmax() && current_power < power_limit;

	bool critical_throttling = t2_triggered || t3_triggered;

	bool new_kernel_nops_enabled =
		((kernel_nops_enabled || start_nops) && !stop_nops) || critical_throttling;

	if (new_kernel_nops_enabled != kernel_nops_enabled) {
		kernel_nops_enabled = new_kernel_nops_enabled;
		SendKernelThrottlingMessage(kernel_nops_enabled);
	}

	EnableArbMax(aiclk_arb_max_doppler_critical, critical_throttling);
}

void CalculateThrottlers(void)
{
	TelemetryInternalData telemetry_internal_data;
	enum aiclk_arb_min arb;

	ReadTelemetryInternal(1, &telemetry_internal_data);

	if (DopplerActive()) {
		UpdateDoppler(&telemetry_internal_data);
	} else {
		UpdateThrottler(kThrottlerTDP, telemetry_internal_data.vcore_power);
		UpdateThrottler(kThrottlerFastTDC, telemetry_internal_data.vcore_current);
		UpdateThrottler(kThrottlerTDC, telemetry_internal_data.vcore_current);
		UpdateThrottler(kThrottlerBoardPower, GetInputPower());

		float current_power = telemetry_internal_data.vcore_power;
		float tdp_limit = throttler[kThrottlerTDP].limit;

		bool start_nops = GetAiclkTarg() == GetAiclkFmin() && current_power > tdp_limit;
		bool stop_nops = GetAiclkTarg() == get_aiclk_effective_arb_min(&arb) &&
				 current_power < tdp_limit;

		bool new_kernel_nops_enabled = ((kernel_nops_enabled || start_nops) && !stop_nops);

		if (new_kernel_nops_enabled != kernel_nops_enabled) {
			kernel_nops_enabled = new_kernel_nops_enabled;
			SendKernelThrottlingMessage(kernel_nops_enabled);
		}
	}

	UpdateThrottler(kThrottlerThm, telemetry_internal_data.asic_temperature);
	UpdateThrottler(kThrottlerGDDRThm, GetMaxGDDRTemp());

	for (ThrottlerId i = 0; i < kThrottlerCount; i++) {
		UpdateThrottlerArb(i);
	}
}

int32_t Dm2CmSetBoardPowerLimit(const uint8_t *data, uint8_t size)
{
	if (size != 2) {
		return -1;
	}

	power_limit = sys_get_le16(data);

	LOG_INF("Cable Power Limit: %u", power_limit);
	power_limit = MIN(power_limit,
			  tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.board_power_limit);

	SetThrottlerLimit(kThrottlerBoardPower, power_limit);
	SetThrottlerLimit(kThrottlerDopplerSlow, power_limit);

	UpdateTelemetryBoardPowerLimit(power_limit);

	return 0;
}

static uint8_t set_tdp_limit_handler(const union request *request, struct response *response)
{
	float default_tdp_limit = get_throttler_clamped_limit(
		kThrottlerTDP, tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit);
	float max_tdp_limit =
		CLAMP(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.max_tdp_limit,
		      default_tdp_limit, throttler_limit_ranges[kThrottlerTDP].max);
	float new_tdp_limit;

	if (request->set_tdp_limit.restore_default) {
		new_tdp_limit = default_tdp_limit;
	} else {
		new_tdp_limit = request->set_tdp_limit.tdp_limit;
	}

	/* Return an error if the new TDP limit is outside of the valid range */
	if (new_tdp_limit > max_tdp_limit) {
		return 1;
	} else if (get_throttler_clamped_limit(kThrottlerTDP, new_tdp_limit) != new_tdp_limit) {
		return 1;
	}

	SetThrottlerLimit(kThrottlerTDP, new_tdp_limit);
	UpdateTelemetryTdpLimit(throttler[kThrottlerTDP].limit);

	return 0;
}

REGISTER_MESSAGE(TT_SMC_MSG_SET_TDP_LIMIT, set_tdp_limit_handler);
