/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cm2dm_msg.h"
#include "dvfs.h"
#include "fan_ctrl.h"
#include "init.h"
#include "reg.h"
#include "smbus_target.h"
#include "status_reg.h"
#include "telemetry.h"
#include "timer.h"

#include <stdint.h>

#include <app_version.h>
#include <tenstorrent/msgqueue.h>
#include <tenstorrent/post_code.h>
#include <tenstorrent/sys_init_defines.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/misc/bh_fwtable.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/dfu/mcuboot.h>

LOG_MODULE_REGISTER(main, CONFIG_TT_APP_LOG_LEVEL);

static const struct device *const wdt0 = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static const struct device *const fwtable_dev = DEVICE_DT_GET(DT_NODELABEL(fwtable));

BUILD_ASSERT(PARTITION_EXISTS(cmfw), "cmfw fixed-partition does not exist");

int main(void)
{
	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ZEPHYR_INIT_DONE);
	printk("Tenstorrent Blackhole CMFW %s\n", APP_VERSION_STRING);

	if (!IS_ENABLED(CONFIG_TT_SMC_RECOVERY)) {
		if (tt_bh_fwtable_get_fw_table(fwtable_dev)->feature_enable.aiclk_ppm_en) {
			STATUS_ERROR_STATUS0_reg_u error_status0 = {
				.val = ReadReg(STATUS_ERROR_STATUS0_REG_ADDR)};

			if (error_status0.f.regulator_init_error) {
				LOG_ERR("Not enabling AICLK PPM due to regulator init error");
			} else {
				/* DVFS should get enabled if AICLK PPM or L2CPUCLK PPM is enabled
				 * We currently don't have plans to implement L2CPUCLK PPM,
				 * so currently, dvfs_enable == aiclk_ppm_enable
				 */
				InitDVFS();
			}
		}
	}

	init_msgqueue();

	if (!IS_ENABLED(CONFIG_TT_SMC_RECOVERY)) {
		init_telemetry(APPVERSION);
		if (tt_bh_fwtable_get_fw_table(fwtable_dev)->feature_enable.fan_ctrl_en) {
			init_fan_ctrl();
		}

		/* These timers are split out from their init functions since their work tasks have
		 * i2c conflicts with other init functions.
		 *
		 * Note: The above issue would be solved by using Zephyr's driver model.
		 */
		StartTelemetryTimer();
		if (dvfs_enabled) {
			StartDVFSTimer();
		}
	}

	Dm2CmReadyRequest();

#ifdef CONFIG_BOOTLOADER_MCUBOOT
	int rc;

	/* For now, if we make it here than we passed the BIST and will confirm the image */
	if (!boot_is_img_confirmed()) {
		rc = boot_write_img_confirmed();
		if (rc < 0) {
			return rc;
		}
		printk("Firmware update is confirmed.\n");
	}
#endif

	while (1) {
		sys_trace_named_event("main_loop", TimerTimestamp(), 0);
		k_msleep(CONFIG_TT_BH_ARC_WDT_FEED_INTERVAL);
		wdt_feed(wdt0, 0);
	}

	return 0;
}

#define FW_VERSION_SEMANTIC APPVERSION
#define FW_VERSION_DATE     0x00000000
#define FW_VERSION_LOW      0x00000000
#define FW_VERSION_HIGH     0x00000000

uint32_t FW_VERSION[4] __attribute__((section(".fw_version"))) = {
	FW_VERSION_SEMANTIC, FW_VERSION_DATE, FW_VERSION_LOW, FW_VERSION_HIGH};

static int tt_appversion_init(void)
{
	WriteReg(STATUS_FW_VERSION_REG_ADDR, APPVERSION);
	return 0;
}
SYS_INIT(tt_appversion_init, EARLY, 0);

static int record_cmfw_start_time(void)
{
	WriteReg(CMFW_START_TIME_REG_ADDR, TimerTimestamp());
	return 0;
}
SYS_INIT(record_cmfw_start_time, EARLY, 0);

static int bh_arc_init_start(void)
{
	/* Write a status register indicating HW init progress */
	STATUS_BOOT_STATUS0_reg_u boot_status0 = {0};

	boot_status0.val = ReadReg(STATUS_BOOT_STATUS0_REG_ADDR);
	boot_status0.f.hw_init_status = kHwInitStarted;
	WriteReg(STATUS_BOOT_STATUS0_REG_ADDR, boot_status0.val);

	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEP1);
	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEP2);

	return 0;
}
SYS_INIT_APP(bh_arc_init_start);

int tt_init_status;

static int bh_arc_init_end(void)
{
	STATUS_BOOT_STATUS0_reg_u boot_status0 = {0};

	/* Indicate successful HW Init */
	boot_status0.val = ReadReg(STATUS_BOOT_STATUS0_REG_ADDR);
	/* Record FW ID */
	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY)) {
		boot_status0.f.fw_id = FW_ID_SMC_RECOVERY;
	} else {
		boot_status0.f.fw_id = FW_ID_SMC_NORMAL;
	}
	boot_status0.f.hw_init_status = (tt_init_status == 0) ? kHwInitDone : kHwInitError;
	WriteReg(STATUS_BOOT_STATUS0_REG_ADDR, boot_status0.val);
	WriteReg(STATUS_ERROR_STATUS0_REG_ADDR, error_status0.val);

	return 0;
}
SYS_INIT_APP(bh_arc_init_end);
