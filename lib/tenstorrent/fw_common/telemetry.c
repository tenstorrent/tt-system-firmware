/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Platform-independent telemetry core: tag accessors, update interval and the periodic update
 * timer. Table layout and data collection live in the per-SoC telemetry_platform implementation.
 * The accessors are always built so message handlers can use them in recovery images; the
 * periodic part requires CONFIG_TT_TELEMETRY_PERIODIC.
 */

#include "telemetry.h"
#include "telemetry_platform.h"

#include <float.h> /* for FLT_MAX */
#include <math.h>  /* for floor */
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <tenstorrent/sys_init_defines.h>
#if defined(HAS_APP_VERSION)
#include <zephyr/app_version.h>
#else
#define APPVERSION 0x00000000
#endif

LOG_MODULE_REGISTER(telemetry, CONFIG_TT_APP_LOG_LEVEL);

void telemetry_set(uint16_t tag, uint32_t value)
{
	int slot = telemetry_platform_slot(tag);

	if (slot >= 0) {
		telemetry_platform_data()[slot] = value;
	}
}

uint32_t telemetry_get(uint16_t tag)
{
	int slot = telemetry_platform_slot(tag);

	return slot >= 0 ? telemetry_platform_data()[slot] : 0;
}

uint32_t ConvertFloatToTelemetry(float value)
{
	/* Convert float to signed int 16.16 format */

	/* Handle error condition */
	if (value == FLT_MAX || value == -FLT_MAX) {
		return 0x80000000;
	}

	float abs_value = fabsf(value);
	uint16_t int_part = floorf(abs_value);
	uint16_t frac_part = (abs_value - int_part) * 65536;
	uint32_t ret_value = (int_part << 16) | frac_part;
	/* Return the 2's complement if the original value was negative */
	if (value < 0) {
		ret_value = -ret_value;
	}
	return ret_value;
}

float ConvertTelemetryToFloat(int32_t value)
{
	/* Convert signed int 16.16 format to float */
	if (value == INT32_MIN) {
		return FLT_MAX;
	} else {
		return value / 65536.0;
	}
}

void UpdateDmFwVersion(uint32_t bl_version, uint32_t app_version)
{
	telemetry_set(TAG_DM_BL_FW_VERSION, bl_version);
	telemetry_set(TAG_DM_APP_FW_VERSION, app_version);
}

void UpdateTelemetryNocTranslation(bool translation_enabled)
{
	/* Note that this may be called before init_telemetry. */
	telemetry_set(TAG_NOC_TRANSLATION, translation_enabled);
}

void UpdateTelemetryBoardPowerLimit(uint32_t power_limit)
{
	telemetry_set(TAG_BOARD_POWER_LIMIT, power_limit);
}

void UpdateTelemetryTdpLimit(uint32_t tdp_limit)
{
	telemetry_set(TAG_TDP_LIMIT_MAX, tdp_limit);
}

void UpdateTelemetryThermTripCount(uint16_t therm_trip_count)
{
	telemetry_set(TAG_THERM_TRIP_COUNT, therm_trip_count);
}

void UpdateTelemetryHostAiclkLimit(uint32_t fmax)
{
	telemetry_set(TAG_HOST_AICLK_LIMIT, fmax);
}

void UpdateTelemetryKernelThrottler(bool enabled, uint32_t stop_nops_freq)
{
	telemetry_feature_flags_0_t active_config = {
		.u32_all = telemetry_get(TAG_FW_ACTIVE_CONFIG_0),
	};

	active_config.bits.kernel_nops_at_aiclk_fmin = enabled ? 1U : 0U;
	telemetry_set(TAG_FW_ACTIVE_CONFIG_0, active_config.u32_all);
	telemetry_set(TAG_KERNEL_THROTTLER,
		      (enabled ? 1U : 0U) | ((stop_nops_freq & 0xFFFFU) << 16U));
}

void UpdateTelemetryFlashJedecId(uint32_t jedec_id)
{
	/* Note that this is called before init_telemetry;
	 * write_static_telemetry must not clear it.
	 */
	telemetry_set(TAG_FLASH_JEDEC_ID, jedec_id);
}

uint32_t GetTelemetryFlashJedecId(void)
{
	return telemetry_get(TAG_FLASH_JEDEC_ID);
}

void UpdateTelemetryGddrThermTrip(bool enabled)
{
	telemetry_feature_flags_0_t active_config = {
		.u32_all = telemetry_get(TAG_FW_ACTIVE_CONFIG_0),
	};

	active_config.bits.gddr_therm_trip = enabled ? 1U : 0U;
	telemetry_set(TAG_FW_ACTIVE_CONFIG_0, active_config.u32_all);
}

telemetry_feature_flags_bits_0_t GetActiveFeatures(void)
{
	telemetry_feature_flags_0_t active_config = {
		.u32_all = telemetry_get(TAG_FW_ACTIVE_CONFIG_0),
	};

	return active_config.bits;
}

bool GetTelemetryTagValid(uint16_t tag)
{
	return telemetry_platform_slot(tag) >= 0;
}

uint32_t GetTelemetryTag(uint16_t tag)
{
	int slot = telemetry_platform_slot(tag);

	if (slot < 0) {
		return -1;
	}
	return telemetry_platform_data()[slot];
}

#if defined(CONFIG_BH_FWTABLE) || defined(CONFIG_TT_GR_SMC)
int init_telemetry(void)
{
	telemetry_platform_write_static(APPVERSION);
	/* fill the dynamic values once before starting timed updates */
	telemetry_platform_update();
	telemetry_platform_publish();

	return 0;
}
SYS_INIT_APP(init_telemetry);
#endif

#ifdef CONFIG_TT_TELEMETRY_PERIODIC
static struct k_timer telem_update_timer;
static struct k_work telem_update_worker;
static int telem_update_interval = TELEM_UPDATE_INTERVAL_DEFAULT_MS;
/* Set once StartTelemetryTimer() has run. Guards against a host interval change arriving
 * between init_telemetry() and StartTelemetryTimer() and starting the timer early.
 */
static bool telem_timer_started;

uint32_t telemetry_update_interval_ms(void)
{
	return telem_update_interval;
}

/* Handler functions for zephyr timer and worker objects */
static void telemetry_work_handler(struct k_work *work)
{
	/* Repeat fetching of dynamic telemetry values */
	telemetry_platform_update();
}
static void telemetry_timer_handler(struct k_timer *timer)
{
	k_work_submit(&telem_update_worker);
}

/* Zephyr timer object submits a work item to the system work queue whose thread performs the task
 * on a periodic basis.
 */
/* See:
 * https://docs.zephyrproject.org/latest/kernel/services/timing/timers.html#using-a-timer-expiry-function
 */
static K_WORK_DEFINE(telem_update_worker, telemetry_work_handler);
static K_TIMER_DEFINE(telem_update_timer, telemetry_timer_handler, NULL);

int StartTelemetryTimer(void)
{
	/* Start the timer to update the dynamic telemetry values
	 * Duration (time interval before the timer expires for the first time) and
	 * Period (time interval between all timer expirations after the first one)
	 * are both set to telem_update_interval.
	 *
	 * Split from init_telemetry because the work task has I2C conflicts with
	 * other init functions. Zephyr's driver model would solve this.
	 */
	k_timer_start(&telem_update_timer, K_MSEC(telem_update_interval),
		      K_MSEC(telem_update_interval));
	telem_timer_started = true;
	return 0;
}

uint8_t TelemetrySetUpdateInterval(uint32_t interval_ms)
{
	if (interval_ms == 0) {
		interval_ms = TELEM_UPDATE_INTERVAL_DEFAULT_MS;
	} else if (interval_ms < TELEM_UPDATE_INTERVAL_MIN_MS) {
		LOG_WRN("telemetry update interval %u ms rejected, must be 0 (restore default of "
			"%d ms) or at least %d ms",
			interval_ms, TELEM_UPDATE_INTERVAL_DEFAULT_MS,
			TELEM_UPDATE_INTERVAL_MIN_MS);
		return 1;
	}

	telem_update_interval = interval_ms;
	/* Report the new rate to readers of the telemetry table. */
	telemetry_set(TAG_UPDATE_TELEM_SPEED, telem_update_interval);

	/* Restart the timer so the new period takes effect immediately. Before
	 * StartTelemetryTimer() runs it will pick the value up on its own.
	 */
	if (telem_timer_started) {
		k_timer_start(&telem_update_timer, K_MSEC(telem_update_interval),
			      K_MSEC(telem_update_interval));
	}

	LOG_INF("telemetry update interval set to %d ms", telem_update_interval);
	return 0;
}
SYS_INIT_APP(StartTelemetryTimer);
#endif /* CONFIG_TT_TELEMETRY_PERIODIC */
