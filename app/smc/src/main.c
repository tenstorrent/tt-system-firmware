/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cm2dm_msg.h"
#include "timer.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/dfu/mcuboot.h>

LOG_MODULE_REGISTER(main, CONFIG_TT_APP_LOG_LEVEL);

static const struct device *const wdt0 = DEVICE_DT_GET(DT_NODELABEL(wdt0));

BUILD_ASSERT(PARTITION_EXISTS(cmfw), "cmfw fixed-partition does not exist");

int main(void)
{
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
