/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tenstorrent Mimir GDDR7 memory-controller driver.
 *
 * One Zephyr device models the whole controller; the tiles it brings up are the
 * status-okay nodes referenced by its mem-tiles. memc_tt_mimir_set_config()
 * fills fw_params.gddr_tile_mask from that DT topology.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/memc/memc_tt_mimir.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "gddr.h"
#include "gddr_drv.h"

LOG_MODULE_REGISTER(memc_tt_mimir, CONFIG_MEMC_LOG_LEVEL);

#define DT_DRV_COMPAT tenstorrent_mimir_memc

/* gddr_tile_mask = OR of BIT(tile-index) over this controller's status-okay mem-tiles */
#define MEMC_TILE_BIT(node_id, prop, idx)                                                          \
	(DT_NODE_HAS_STATUS(DT_PHANDLE_BY_IDX(node_id, prop, idx), okay)                           \
		 ? BIT(DT_PROP_BY_PHANDLE_IDX(node_id, prop, idx, tile_index))                     \
		 : 0)
#define MEMC_GDDR_TILE_MASK DT_INST_FOREACH_PROP_ELEM_SEP(0, mem_tiles, MEMC_TILE_BIT, (|))

struct memc_tt_mimir_data {
	gddr_config_t cfg;
};

int memc_tt_mimir_set_config(const struct device *dev, const gddr_backend_t *backend,
			     fw_params_t *params)
{
	struct memc_tt_mimir_data *data = dev->data;

	if (backend == NULL || params == NULL) {
		return -EINVAL;
	}

	params->chiplet.mimir_smc.gddr.gddr_tile_mask = MEMC_GDDR_TILE_MASK;

	data->cfg.params = params;
	data->cfg.backend = backend;
	/* irq_register/irq_ctx left zero: RAS IRQ unarmed, no consumer yet. */

	return 0;
}

static int memc_tt_mimir_init(const struct device *dev)
{
	struct memc_tt_mimir_data *data = dev->data;
	grendel_err_t err;

	if (data->cfg.params == NULL || data->cfg.params->magic != FW_PARAMS_MAGIC ||
	    data->cfg.backend == NULL || data->cfg.backend->get_blob == NULL) {
		LOG_ERR("GDDR config not installed before memc init");
		return -EINVAL;
	}

	err = gddr_init(&data->cfg);
	if (err != GRENDEL_ERR_OK) {
		LOG_ERR("gddr_init failed: %s", grendel_err_to_str(err));
		return -EIO;
	}

	LOG_INF("GDDR bring-up complete (tile_mask=0x%x)", (unsigned int)gddr_get_tile_en());
	return 0;
}

static struct memc_tt_mimir_data memc_tt_mimir_data0;

DEVICE_DT_INST_DEFINE(0, memc_tt_mimir_init, NULL, &memc_tt_mimir_data0, NULL, POST_KERNEL,
		      CONFIG_MEMC_TT_MIMIR_INIT_PRIORITY, NULL);
