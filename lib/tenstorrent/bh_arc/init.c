/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cat.h"
#include "dvfs.h"
#include "fan_ctrl.h"
#include "init.h"
#include "reg.h"
#include "status_reg.h"
#include "telemetry.h"
#include "timer.h"
#include "cm2dm_msg.h"
#include <stdint.h>

#if defined(HAS_APP_VERSION)
#include <zephyr/app_version.h>
#else
#define APPVERSION         0x00000000
#define APP_VERSION_STRING "unknown"
#endif

#include <tenstorrent/msgqueue.h>
#include <tenstorrent/post_code.h>
#include <tenstorrent/sys_init_defines.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#ifdef CONFIG_BH_FWTABLE
#include <zephyr/drivers/misc/bh_fwtable.h>
#endif

LOG_MODULE_REGISTER(bh_arc_init, CONFIG_TT_APP_LOG_LEVEL);

#ifdef CONFIG_BH_FWTABLE
static const struct device *const fwtable_dev = DEVICE_DT_GET(DT_NODELABEL(fwtable));
#endif

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

	boot_status0.f.hw_init_status = (error_status0 != 0) ? kHwInitError : kHwInitDone;
	WriteReg(STATUS_BOOT_STATUS0_REG_ADDR, boot_status0.val);
	WriteReg(STATUS_ERROR_STATUS0_REG_ADDR, error_status0);

	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ZEPHYR_INIT_DONE);
	printk("Tenstorrent Blackhole CMFW %s\n", APP_VERSION_STRING);

	Dm2CmReadyRequest();
	return 0;
}
SYS_INIT_APP(bh_arc_init_end);
