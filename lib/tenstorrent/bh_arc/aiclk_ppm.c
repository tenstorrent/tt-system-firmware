/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aiclk_ppm.h"
#include "dvfs.h"
#include "telemetry.h"
#include "throttler.h"
#include "voltage.h"
#include "vf_curve.h"

#include <stdlib.h>

#include <tenstorrent/bh_power.h>
#include <tenstorrent/smc_msg.h>
#include <tenstorrent/msgqueue.h>
#include <tenstorrent/sys_init_defines.h>
#include <zephyr/init.h>
#include <zephyr/drivers/misc/bh_fwtable.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control/clock_control_tt_bh.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/logging/log.h>
#include <zephyr/tracing/tracing.h>

LOG_MODULE_REGISTER(aiclk_ppm, CONFIG_TT_APP_LOG_LEVEL);

static const struct device *const pll_dev_0 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(pll0));

/* aiclk control mode */
typedef enum {
	CLOCK_MODE_UNCONTROLLED = 1,
	CLOCK_MODE_PPM_FORCED = 2,
	CLOCK_MODE_PPM_UNFORCED = 3
} ClockControlMode;

typedef struct {
	bool enabled;
	float value;
} AiclkArb;

typedef struct {
	uint32_t curr_freq;   /* in MHz */
	uint32_t targ_freq;   /* in MHz */
	uint32_t boot_freq;   /* in MHz */
	uint32_t fmax;        /* in MHz */
	uint32_t fmin;        /* in MHz */
	uint32_t host_requested_fmin; /* Host-requested minimum frequency floor, 0 = disabled */
	uint32_t forced_freq; /* in MHz, a value of zero means disabled. */
	bool reset_safe;      /* cap to reset-safe frequency after forced frequency is applied */
	uint32_t sweep_en;    /* a value of one means enabled, otherwise disabled. */
	uint32_t sweep_low;   /* in MHz */
	uint32_t sweep_high;  /* in MHz */
	union aiclk_targ_freq_info lim_arb_info; /*information on the limiting arbiter */
	AiclkArb arbiter_max[aiclk_arb_max_count];
	AiclkArb arbiter_min[aiclk_arb_min_count];
} AiclkPPM;

static AiclkPPM aiclk_ppm = {
	.fmax = AICLK_FMAX_MAX,
	.fmin = AICLK_FMIN_MIN,
};

static const struct device *const fwtable_dev = DEVICE_DT_GET(DT_NODELABEL(fwtable));

static bool last_msg_busy;

static uint32_t final_arbiter_count[aiclk_arb_max_count];
static uint32_t throttler_frozen_mask;
static uint32_t throttler_overflow_mask;

void SetAiclkArbMax(enum aiclk_arb_max arb_max, float freq)
{
	aiclk_ppm.arbiter_max[arb_max].value = CLAMP(freq, aiclk_ppm.fmin, aiclk_ppm.fmax);
}

void SetAiclkArbMin(enum aiclk_arb_min arb_min, float freq)
{
	aiclk_ppm.arbiter_min[arb_min].value = CLAMP(freq, aiclk_ppm.fmin, aiclk_ppm.fmax);
}

void EnableArbMax(enum aiclk_arb_max arb_max, bool enable)
{
	aiclk_ppm.arbiter_max[arb_max].enabled = enable;
}

void EnableArbMin(enum aiclk_arb_min arb_min, bool enable)
{
	aiclk_ppm.arbiter_min[arb_min].enabled = enable;
}

void CalculateTargAiclk(void)
{
	/* Calculate the target AICLK frequency */
	/* Start by calculating the highest arbiter_min */
	/* Then limit to the lowest arbiter_max */
	/* Finally make sure that the target frequency is at least Fmin */

	enum aiclk_arb_min min_arb = aiclk_arb_min_count;
	enum aiclk_arb_max max_arb = aiclk_arb_max_count;

	aiclk_ppm.targ_freq = get_aiclk_effective_arb_min(&min_arb);

	union aiclk_targ_freq_info info = {.reason = limit_reason_min_arb, .arbiter = min_arb};

	uint32_t max_arb_freq = get_aiclk_effective_arb_max(&max_arb);

	if (aiclk_ppm.targ_freq > max_arb_freq) {
		aiclk_ppm.targ_freq = max_arb_freq;
		info.reason = limit_reason_max_arb;
		info.arbiter = max_arb;
	}

	/* Throttling only if we are below Fmax and busy arbiter is at Fmax */
	bool throttling = (aiclk_ppm.targ_freq != aiclk_ppm.fmax);
	bool aiclk_busy = (aiclk_ppm.arbiter_min[aiclk_arb_min_busy].value == aiclk_ppm.fmax);

	for (enum aiclk_arb_max i = 0; i < aiclk_arb_max_count; ++i) {
		bool arbiter_enabled = aiclk_ppm.arbiter_max[i].enabled;

		if (arbiter_enabled && aiclk_ppm.arbiter_max[i].value == aiclk_ppm.targ_freq &&
		    throttling && aiclk_busy && !(throttler_frozen_mask & BIT(i))) {
			if (final_arbiter_count[i] < UINT32_MAX) {
				final_arbiter_count[i]++;
			} else {
				throttler_overflow_mask |= BIT(i);
			}
		}
	}

	/* Make sure target is not below Fmin or host-requested minimum */
	/* (it will not be above Fmax, since we calculated the max limits last) */
	uint32_t effective_fmin_floor = aiclk_ppm.fmin;

	if (aiclk_ppm.host_requested_fmin > 0) {
		effective_fmin_floor = MAX(effective_fmin_floor, aiclk_ppm.host_requested_fmin);
	}
	if (aiclk_ppm.targ_freq < effective_fmin_floor) {
		aiclk_ppm.targ_freq = effective_fmin_floor;
		info.reason = limit_reason_fmin;
		info.arbiter = 0U;
	}

	/* Apply random frequency if sweep is enabled */
	if (aiclk_ppm.sweep_en == 1) {
		aiclk_ppm.targ_freq = rand() % (aiclk_ppm.sweep_high - aiclk_ppm.sweep_low + 1) +
				      aiclk_ppm.sweep_low;

		info.reason = limit_reason_sweep;
		info.arbiter = 0U;
	}

	/* Apply forced frequency at the end, regardless of any limits */
	if (aiclk_ppm.forced_freq != 0) {
		aiclk_ppm.targ_freq = aiclk_ppm.forced_freq;
		info.reason = limit_reason_forced;
		info.arbiter = 0U;
	}

	if (aiclk_ppm.reset_safe && aiclk_ppm.targ_freq > AICLK_RESET_SAFE_FREQ) {
		aiclk_ppm.targ_freq = AICLK_RESET_SAFE_FREQ;
	}

	aiclk_ppm.lim_arb_info = info;
	sys_trace_named_event("targ_freq_update", aiclk_ppm.targ_freq,
			      aiclk_ppm.lim_arb_info.u32_all);
}

void DecreaseAiclk(void)
{
	if (aiclk_ppm.targ_freq < aiclk_ppm.curr_freq) {
		clock_control_set_rate(pll_dev_0,
				       (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_AICLK,
				       (clock_control_subsys_rate_t)aiclk_ppm.targ_freq);
		aiclk_ppm.curr_freq = aiclk_ppm.targ_freq;
		sys_trace_named_event("aiclk_update", aiclk_ppm.curr_freq, aiclk_ppm.targ_freq);
	}
}

void IncreaseAiclk(void)
{
	if (aiclk_ppm.targ_freq > aiclk_ppm.curr_freq) {
		clock_control_set_rate(pll_dev_0,
				       (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_AICLK,
				       (clock_control_subsys_rate_t)aiclk_ppm.targ_freq);
		aiclk_ppm.curr_freq = aiclk_ppm.targ_freq;
		sys_trace_named_event("aiclk_update", aiclk_ppm.curr_freq, aiclk_ppm.targ_freq);
	}
}

float GetThrottlerArbMax(enum aiclk_arb_max arb_max)
{
	return aiclk_ppm.arbiter_max[arb_max].value;
}

/* TODO: Write a Zephyr unit test for this function */
uint32_t GetMaxAiclkForVoltage(uint32_t voltage)
{
	/* Assume monotonically increasing relationship between frequency and voltage */
	/* and conduct binary search. */
	/* Note this function doesn't work if you would need lower than fmin to achieve the voltage
	 */

	/* starting high_freq at fmax + 1 solves the case where the Max AICLK is fmax */
	uint32_t high_freq = aiclk_ppm.fmax + 1;
	uint32_t low_freq = aiclk_ppm.fmin;

	while (low_freq < high_freq) {
		uint32_t mid_freq = (low_freq + high_freq) / 2;

		if (VFCurve(mid_freq) > voltage) {
			high_freq = mid_freq;
		} else {
			low_freq = mid_freq + 1;
		}
	}

	return low_freq - 1;
}

void InitArbMaxVoltage(void)
{
	/* ArbMaxVoltage is statically set to the frequency of the maximum voltage */
	SetAiclkArbMax(aiclk_arb_max_voltage, GetMaxAiclkForVoltage(voltage_arbiter.vdd_max));
}

static int InitAiclkPPM(void)
{
	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY)) {
		return 0;
	}

	/* Initialize some AICLK tracking variables */
	clock_control_get_rate(pll_dev_0, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_AICLK,
			       &aiclk_ppm.boot_freq);

	aiclk_ppm.curr_freq = aiclk_ppm.boot_freq;
	aiclk_ppm.targ_freq = aiclk_ppm.curr_freq;

	if (IS_ENABLED(CONFIG_ARC)) {
		aiclk_ppm.fmax =
			CLAMP(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.asic_fmax,
			      AICLK_FMAX_MIN, AICLK_FMAX_MAX);
		aiclk_ppm.fmin =
			CLAMP(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.asic_fmin,
			      AICLK_FMIN_MIN, AICLK_FMIN_MAX);
	}

	/* disable forcing of AICLK */
	aiclk_ppm.forced_freq = 0;
	aiclk_ppm.reset_safe = false;

	/* disable AICLK sweep */
	aiclk_ppm.sweep_en = 0;

	for (int i = 0; i < aiclk_arb_max_count; i++) {
		aiclk_ppm.arbiter_max[i].value = aiclk_ppm.fmax;
		aiclk_ppm.arbiter_max[i].enabled = true;
	}

	for (int i = 0; i < aiclk_arb_min_count; i++) {
		aiclk_ppm.arbiter_min[i].value = aiclk_ppm.fmin;
		aiclk_ppm.arbiter_min[i].enabled = true;
	}
	/* Disable aiclk_arb_max_host_fmax at init; only activated when
	 * TT_SMC_MSG_SET_ASIC_HOST_FMAX is received.
	 */
	EnableArbMax(aiclk_arb_max_host_fmax, false);
	return 0;
}
SYS_INIT_APP(InitAiclkPPM);

uint8_t ForceAiclk(uint32_t freq)
{
	if ((freq > AICLK_FMAX_MAX || freq < AICLK_FMIN_MIN) && (freq != 0)) {
		return 1;
	}

	if (dvfs_enabled) {
		aiclk_ppm.forced_freq = freq;
		DVFSChange();
	} else {
		/* restore to boot frequency */
		if (freq == 0) {
			freq = aiclk_ppm.boot_freq;
		}

		clock_control_set_rate(pll_dev_0,
				       (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_AICLK,
				       (clock_control_subsys_rate_t)freq);
	}
	return 0;
}

void SetAiclkResetSafe(bool enable)
{
	if (aiclk_ppm.reset_safe == enable) {
		return;
	}

	aiclk_ppm.reset_safe = enable;
	DVFSChange();
}

uint32_t GetAiclkTarg(void)
{
	return aiclk_ppm.targ_freq;
}

uint32_t GetAiclkFmin(void)
{
	return aiclk_ppm.fmin;
}

uint32_t GetAiclkFmax(void)
{
	return aiclk_ppm.fmax;
}

void aiclk_update_busy(void)
{
	bool aiclk_state;

	bh_power_state_get(BH_POWER_DOMAIN_AICLK, &aiclk_state);

	if (last_msg_busy || aiclk_state) {
		SetAiclkArbMin(aiclk_arb_min_busy, aiclk_ppm.fmax);
	} else {
		SetAiclkArbMin(aiclk_arb_min_busy, aiclk_ppm.fmin);
	}
}

uint32_t get_aiclk_effective_arb_min(enum aiclk_arb_min *effective_min_arb)
{
	/* Calculate the highest enabled arbiter_min */
	uint32_t effective_min = aiclk_ppm.fmin;

	for (enum aiclk_arb_min i = 0; i < aiclk_arb_min_count; i++) {
		if (aiclk_ppm.arbiter_min[i].enabled) {
			if (aiclk_ppm.arbiter_min[i].value >= effective_min) {
				effective_min = aiclk_ppm.arbiter_min[i].value;
				*effective_min_arb = i;
			}
		}
	}

	return effective_min;
}

uint32_t get_aiclk_effective_arb_max(enum aiclk_arb_max *effective_max_arb)
{
	/* Calculate the lowest enabled arbiter_max */
	uint32_t effective_max = aiclk_ppm.fmax;

	for (enum aiclk_arb_max i = 0; i < aiclk_arb_max_count; i++) {
		if (aiclk_ppm.arbiter_max[i].enabled) {
			if (aiclk_ppm.arbiter_max[i].value <= effective_max) {
				effective_max = aiclk_ppm.arbiter_max[i].value;
				*effective_max_arb = i;
			}
		}
	}

	return effective_max;
}

uint32_t get_enabled_arb_min_bitmask(void)
{
	/* Return a bitmask of enabled min arbiters */
	uint32_t bitmask = 0;

	for (enum aiclk_arb_min i = 0; i < aiclk_arb_min_count; i++) {
		if (aiclk_ppm.arbiter_min[i].enabled) {
			bitmask |= (1 << i);
		}
	}

	return bitmask;
}

uint32_t get_enabled_arb_max_bitmask(void)
{
	/* Return a bitmask of enabled max arbiters */
	uint32_t bitmask = 0;

	for (enum aiclk_arb_max i = 0; i < aiclk_arb_max_count; i++) {
		if (aiclk_ppm.arbiter_max[i].enabled) {
			bitmask |= (1 << i);
		}
	}

	return bitmask;
}

/** @brief Handles the request to set AICLK busy or idle
 * @param[in] request The request, of type @ref aiclk_set_speed_rqst, with command code
 *	@ref TT_SMC_MSG_AICLK_GO_BUSY to go busy, or @ref TT_SMC_MSG_AICLK_GO_LONG_IDLE to go idle.
 * @param[out] response The response to the host
 * @return 0 for success
 */
static uint8_t aiclk_busy_handler(const union request *request, struct response *response)
{
	last_msg_busy = (request->aiclk_set_speed.command_code == TT_SMC_MSG_AICLK_GO_BUSY);
	aiclk_update_busy();
	return 0;
}

/**
 * @brief Handler for @ref TT_SMC_MSG_FORCE_AICLK
 * @see force_aiclk_rqst
 */
static uint8_t ForceAiclkHandler(const union request *request, struct response *response)
{
	uint32_t forced_freq = request->force_aiclk.forced_freq;

	return ForceAiclk(forced_freq);
}

/**
 * @brief Handler for @ref TT_SMC_MSG_GET_AICLK
 * @see get_aiclk_rqst
 */
static uint8_t get_aiclk_handler(const union request *request, struct response *response)
{
	clock_control_get_rate(pll_dev_0, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_AICLK,
			       &(response->data[1]));

	if (!dvfs_enabled) {
		response->data[2] = CLOCK_MODE_UNCONTROLLED;
	} else if (aiclk_ppm.forced_freq != 0) {
		response->data[2] = CLOCK_MODE_PPM_FORCED;
	} else {
		response->data[2] = CLOCK_MODE_PPM_UNFORCED;
	}

	return 0;
}

/**
 * @brief Handler for @ref TT_SMC_MSG_AISWEEP_START and @ref TT_SMC_MSG_AISWEEP_STOP
 * @see aisweep_rqst
 */
static uint8_t SweepAiclkHandler(const union request *request, struct response *response)
{
	if (request->command_code == TT_SMC_MSG_AISWEEP_START) {
		if (request->aisweep.sweep_low == 0 || request->aisweep.sweep_high == 0) {
			return 1;
		}
		aiclk_ppm.sweep_low = MAX(request->aisweep.sweep_low, aiclk_ppm.fmin);
		aiclk_ppm.sweep_high = MIN(request->aisweep.sweep_high, aiclk_ppm.fmax);
		aiclk_ppm.sweep_en = 1;
	} else {
		aiclk_ppm.sweep_en = 0;
	}
	return 0;
}

union aiclk_targ_freq_info get_targ_aiclk_info(void)
{
	return aiclk_ppm.lim_arb_info;
}

static uint8_t set_arb_host_fmax_handler(const union request *request, struct response *response)
{
	uint32_t new_fmax;

	if (request->set_asic_host_fmax.restore_default) {
		/* Disable the host_fmax arbiter */
		EnableArbMax(aiclk_arb_max_host_fmax, false);
		UpdateTelemetryHostAiclkLimit(0);
		LOG_INF("host fmax arbiter disabled");
		return 0;
	}

	new_fmax = request->set_asic_host_fmax.asic_fmax;

	/* Reject if outside valid range [AICLK_FMAX_MIN, AICLK_FMAX_MAX] */
	if (new_fmax > (uint32_t)AICLK_FMAX_MAX || new_fmax < (uint32_t)AICLK_FMAX_MIN) {
		return 1;
	}

	EnableArbMax(aiclk_arb_max_host_fmax, true);
	SetAiclkArbMax(aiclk_arb_max_host_fmax, (float)new_fmax);
	UpdateTelemetryHostAiclkLimit(new_fmax);
	LOG_INF("host fmax arbiter enabled, host fmax set to %u MHz", new_fmax);
	return 0;
}

/** @brief Handles the characterization submessage for setting host minimum frequency floor
 * @param[in] fmin_value The submessage data of type @ref characterisation_set_fmin_submsg
 * @param[out] response The response to the host
 * @return 0 for success, 1 for invalid input
 */
static uint8_t handle_char_set_host_fmin(const struct characterisation_set_fmin_submsg fmin_value,
					 struct response *response)
{
	uint32_t new_fmin = fmin_value.value;

	/* Check for restore flag */
	if (new_fmin == 1) {
		aiclk_ppm.host_requested_fmin = 0;
		LOG_INF("host fmin floor disabled");
		return 0;
	}

	/* Reject if outside valid range [AICLK_FMIN_MIN, AICLK_FMIN_MAX] */
	if (new_fmin > (uint32_t)AICLK_FMIN_MAX || new_fmin < (uint32_t)AICLK_FMIN_MIN) {
		return 1;
	}

	aiclk_ppm.host_requested_fmin = new_fmin;
	LOG_INF("host fmin floor set to %u MHz", new_fmin);
	return 0;
}

/** @brief Dispatcher for characterization submessages
 * @param[in] request The characterization request with submsg_ID
 * @param[out] response The response to the host
 * @return 0 for success, 1 for invalid input or unknown submessage
 */
static uint8_t characterisation_handler(const union request *request, struct response *response)
{
	switch (request->characterisation_msg.submsg_ID) {
	case TT_SUB_MSG_SET_HOST_REQUESTED_FMIN:
		return handle_char_set_host_fmin(
			request->characterisation_msg.submsg_data.fmin_value, response);

	case TT_SUB_MSG_SET_KERNEL_THROTTLER_ENABLED:
		return ThrottlerSetKernelThrottlerEnabled(
			request->characterisation_msg.submsg_data.throttler_enabled.enabled);

	case TT_SUB_MSG_SET_KERNEL_THROTTLER_STOP_NOPS_FREQ:
		return ThrottlerSetKernelThrottlerStopFreq(
			request->characterisation_msg.submsg_data.throttler_stop_freq.frequency);

	default:
		LOG_WRN("Unknown characterization submessage ID: 0x%02x",
			request->characterisation_msg.submsg_ID);
		return 1;
	}
}

uint8_t throttler_counter_handler(const union request *request, struct response *response)
{
	switch (request->counter.command) {
	case COUNTER_CMD_GET: {
		uint32_t idx = request->counter.bank_index;

		if (idx >= aiclk_arb_max_count) {
			return 1;
		}

		uint32_t ovf = (throttler_overflow_mask & BIT(idx)) ? 1U : 0U;
		uint32_t frz = (throttler_frozen_mask & BIT(idx)) ? 1U : 0U;

		response->data[1] = ovf | (frz << 16);
		response->data[2] = final_arbiter_count[idx];
		break;
	}
	case COUNTER_CMD_CLEAR:
		for (uint32_t i = 0; i < aiclk_arb_max_count; i++) {
			if (request->counter.mask & BIT(i)) {
				final_arbiter_count[i] = 0;
				throttler_overflow_mask &= ~BIT(i);
			}
		}
		break;
	case COUNTER_CMD_FREEZE:
		throttler_frozen_mask = request->counter.mask;
		break;
	default:
		return 1;
	}

	return 0;
}

REGISTER_MESSAGE(TT_SMC_MSG_AICLK_GO_BUSY, aiclk_busy_handler);
REGISTER_MESSAGE(TT_SMC_MSG_AICLK_GO_LONG_IDLE, aiclk_busy_handler);
REGISTER_MESSAGE(TT_SMC_MSG_FORCE_AICLK, ForceAiclkHandler);
REGISTER_MESSAGE(TT_SMC_MSG_GET_AICLK, get_aiclk_handler);
REGISTER_MESSAGE(TT_SMC_MSG_AISWEEP_START, SweepAiclkHandler);
REGISTER_MESSAGE(TT_SMC_MSG_AISWEEP_STOP, SweepAiclkHandler);
REGISTER_MESSAGE(TT_SMC_MSG_SET_ASIC_HOST_FMAX, set_arb_host_fmax_handler);
REGISTER_MESSAGE(TT_SMC_MSG_CHARACTERISATION, characterisation_handler);
