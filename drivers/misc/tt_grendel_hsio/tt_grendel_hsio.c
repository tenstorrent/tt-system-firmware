/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/drivers/misc/tt_grendel_hsio.h>

#ifdef CONFIG_SOC_TT_KERAUNOS_SMC
#include <zephyr/init.h>
#endif

#ifdef CONFIG_LOG
#include <zephyr/logging/log.h>
#else
#define LOG_ERR(...)
#define LOG_INF(...)
#define LOG_MODULE_REGISTER(...)
#endif

#include <basic_init.h>
#include <clock.h>
#include <err.h>
#include <firewall.h>
#include <platform.h>

#ifndef CONFIG_TT_GRENDEL_HSIO_LOG_LEVEL
#define CONFIG_TT_GRENDEL_HSIO_LOG_LEVEL 0
#endif

LOG_MODULE_REGISTER(tt_grendel_hsio, CONFIG_TT_GRENDEL_HSIO_LOG_LEVEL);

#define HSIO_TILE_MAX 4U

int tt_grendel_hsio_init(uint32_t hsio_tile)
{
	grendel_err_t err;

	if (hsio_tile > HSIO_TILE_MAX) {
		return -EINVAL;
	}

	smc_release_reset(KER_RST_HSIO0 << hsio_tile);

	enable_hsio_clk(hsio_tile, 0);

	err = disable_firewall_filters(KER_SMN_HSIO2HSIO, hsio_tile);
	if (err != GRENDEL_ERR_OK) {
		LOG_ERR("Failed to open HSIO2HSIO firewall: %d", err);
		return -EIO;
	}

	err = disable_firewall_filters(KER_SMN_HSIO2SMN, hsio_tile);
	if (err != GRENDEL_ERR_OK) {
		LOG_ERR("Failed to open HSIO2SMN firewall: %d", err);
		return -EIO;
	}

	enable_hsio_clk(hsio_tile, 1);

	err = disable_ker_hsio_filters(hsio_tile);
	if (err != GRENDEL_ERR_OK) {
		LOG_ERR("Failed to open HSIO firewall filters: %d", err);
		return -EIO;
	}

	LOG_INF("HSIO%u clocks and filters open", hsio_tile);
	return 0;
}

#ifdef CONFIG_SOC_TT_KERAUNOS_SMC
static int tt_grendel_hsio_sys_init(void)
{
	return tt_grendel_hsio_init(0);
}

SYS_INIT(tt_grendel_hsio_sys_init, POST_KERNEL, CONFIG_TT_GRENDEL_HSIO_INIT_PRIORITY);
#endif
