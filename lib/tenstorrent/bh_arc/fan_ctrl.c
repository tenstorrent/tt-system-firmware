/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fan_ctrl.h"

#include "cm2dm_msg.h"
#include "gddr.h"
#include "telemetry_internal.h"
#include "telemetry.h"
#include "timer.h"
#include "harvesting.h"

#include <tenstorrent/msgqueue.h>
#include <tenstorrent/smc_msg.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/misc/bh_fwtable.h>

#ifdef CONFIG_ZTEST
#define STATIC
#else
#define STATIC static
#endif

LOG_MODULE_REGISTER(fan_ctrl, CONFIG_TT_APP_LOG_LEVEL);

static struct k_timer fan_ctrl_update_timer;
static struct k_work fan_ctrl_update_worker;
static int fan_ctrl_update_interval = 1000;

static uint16_t fan_rpm;   /* Fan RPM from tach */
static uint32_t fan_speed; /* % */
static bool fan_speed_forced;

static uint32_t fan_speed_feedback; /* %, feedback from DMC for telemetry */

static float max_gddr_temp;
static float max_asic_temp;
static float alpha = CONFIG_TT_BH_ARC_FAN_CTRL_ALPHA / 100.0f;

static const struct device *const fwtable_dev = DEVICE_DT_GET(DT_NODELABEL(fwtable));

STATIC uint32_t fan_curve(float max_asic_temp, float max_gddr_temp)
{
	/* P150 fan curve: could be a part of device tree once added to the driver model */
	uint32_t fan_speed1;
	uint32_t fan_speed2;

	if (max_asic_temp < 60) {
		fan_speed1 = 20;
	} else if (max_asic_temp < 90) {
		if (max_asic_temp < 72) {
			fan_speed1 = (uint32_t)(0.833f * max_asic_temp - 29.98f);
		} else if (max_asic_temp < 75) {
			fan_speed1 = (uint32_t)(3.333f * max_asic_temp - 209.976f);
		} else {
			fan_speed1 = (uint32_t)(4.0f * max_asic_temp - 260.0f);
		}
	} else {
		fan_speed1 = 100;
	}

	if (max_gddr_temp < 45) {
		fan_speed2 = 20;
	} else if (max_gddr_temp < 82) {
		if (max_gddr_temp < 60) {
			fan_speed2 = (uint32_t)(0.667f * max_gddr_temp - 10.015f);
		} else if (max_gddr_temp < 65) {
			fan_speed2 = (uint32_t)(2.0f * max_gddr_temp - 90.0f);
		} else {
			fan_speed2 = (uint32_t)(3.529f * max_gddr_temp - 189.385f);
		}
	} else {
		fan_speed2 = 100;
	}

	return MAX(fan_speed1, fan_speed2);
}

static void update_fan_speed(void)
{
	TelemetryInternalData telemetry_internal_data;

	/* Telemetry & computations continue to run even when speed is forced so that they stay
	 * up-to-date.
	 */
	ReadTelemetryInternal(1, &telemetry_internal_data);
	max_asic_temp =
		alpha * telemetry_internal_data.asic_temperature + (1 - alpha) * max_asic_temp;

	if (IS_ENABLED(CONFIG_TT_BH_ARC_FAN_CTRL_GDDR_TEMP)) {
		max_gddr_temp = alpha * GetMaxGDDRTemp() + (1 - alpha) * max_gddr_temp;
	} else {
		max_gddr_temp = 0;
	}

	if (!fan_speed_forced) {
		fan_speed = fan_curve(max_asic_temp, max_gddr_temp);
		UpdateFanSpeedRequest(fan_speed);
	}
}

uint16_t GetFanRPM(void)
{
	return fan_rpm;
}

void SetFanRPM(uint16_t rpm)
{
	fan_rpm = rpm;
}

uint32_t GetFanSpeed(void)
{
	return fan_speed_feedback ? fan_speed_feedback : fan_speed;
}

static void fan_ctrl_work_handler(struct k_work *work)
{
	/* do the processing that needs to be done periodically */
	update_fan_speed();
}
static K_WORK_DEFINE(fan_ctrl_update_worker, fan_ctrl_work_handler);

static void fan_ctrl_timer_handler(struct k_timer *timer)
{
	k_work_submit(&fan_ctrl_update_worker);
}
static K_TIMER_DEFINE(fan_ctrl_update_timer, fan_ctrl_timer_handler, NULL);

void init_fan_ctrl(void)
{
	/* Get initial asic temp */
	TelemetryInternalData telemetry_internal_data;

	ReadTelemetryInternal(1, &telemetry_internal_data);
	max_asic_temp = telemetry_internal_data.asic_temperature;

	/* start a periodic timer that expires once every fan_ctrl_update_interval */
	k_timer_start(&fan_ctrl_update_timer, K_MSEC(fan_ctrl_update_interval),
		      K_MSEC(fan_ctrl_update_interval));
}

static uint8_t force_fan_speed(const union request *request, struct response *response)
{
	if (!tt_bh_fwtable_get_fw_table(fwtable_dev)->feature_enable.fan_ctrl_en) {
		return 1;
	}

	uint32_t raw_speed = request->force_fan_speed.raw_speed;

	if (raw_speed == UINT32_MAX) {
		fan_speed_forced = false;
		fan_speed = fan_curve(max_asic_temp, max_gddr_temp);
	} else {
		fan_speed_forced = true;
		fan_speed = CLAMP(raw_speed, 0, 100);
	}

	if (fan_speed_forced) {
		UpdateForcedFanSpeedRequest(fan_speed);
	} else {
		UpdateFanSpeedRequest(fan_speed);
	}

	return 0;
}
REGISTER_MESSAGE(TT_SMC_MSG_FORCE_FAN_SPEED, force_fan_speed);

void DmcFanSpeedFeedback(uint32_t speed_percentage)
{
	fan_speed_feedback = speed_percentage;
}
