/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, CONFIG_TT_APP_LOG_LEVEL);

int main(void)
{
	LOG_INF("Mission FW Main Loop entry - " CONFIG_BOARD_TARGET);

	while (1) {
		k_msleep(100);
	}

	return 0;
}
