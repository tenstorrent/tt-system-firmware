/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>

#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include "mis_launch.h"

LOG_MODULE_REGISTER(main, CONFIG_TT_APP_LOG_LEVEL);

int main(void)
{
	LOG_INF("BL1 - " CONFIG_BOARD_TARGET);
	launch_mis();

	/* Unreachable unless MIS returns, which means the handoff failed and the core's
	 * state (stack, vector table, PMP, ...) can no longer be trusted.
	 */
	k_panic();
	return 0;
}
