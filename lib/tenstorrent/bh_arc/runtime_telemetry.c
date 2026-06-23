/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "reg.h"
#include "status_reg.h"

#include <stdint.h>
#include <tenstorrent/sys_init_defines.h>

#include <zephyr/init.h>
#include <zephyr/sys/util.h>

#define RUNTIME_TELEMETRY_SIZE CONFIG_TT_BH_ARC_RUNTIME_TELEMETRY_SIZE

BUILD_ASSERT(RUNTIME_TELEMETRY_SIZE > 0);
BUILD_ASSERT(RUNTIME_TELEMETRY_SIZE % 32 == 0);

uint8_t tt_runtime_telemetry[RUNTIME_TELEMETRY_SIZE] __aligned(32)
__attribute__((section(".bss.runtime_telemetry")));

static int publish_runtime_telemetry(void)
{
	WriteReg(RUNTIME_TELEMETRY_ADDR_REG_ADDR, (uint32_t)tt_runtime_telemetry);
	WriteReg(RUNTIME_TELEMETRY_SIZE_REG_ADDR, RUNTIME_TELEMETRY_SIZE);
	return 0;
}

SYS_INIT_APP(publish_runtime_telemetry);
