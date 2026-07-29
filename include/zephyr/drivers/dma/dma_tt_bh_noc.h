/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/dma.h>

enum tt_bh_dma_noc_channel_direction {
	TT_BH_DMA_NOC_CHANNEL_DIRECTION_BROADCAST = DMA_CHANNEL_DIRECTION_PRIV_START
};

struct tt_bh_dma_noc_coords {
	uint8_t source_x, source_y;
	uint8_t dest_x, dest_y;
};
typedef int (*tt_bh_dma_api_config)(const struct device *dev, uint32_t channel,
				    struct dma_config *config, struct tt_bh_dma_noc_coords *coords);
__subsystem struct tt_bh_dma_noc_driver_api {
	/**
	 * For backward compatibility to DMA API.
	 *
	 * @see dma_driver_api for more information.
	 *
	 * @internal
	 * @warning DO NOT MOVE! Must be at the beginning.
	 * @endinternal
	 */
	struct dma_driver_api dma_api;

	/**
	 * API for configuring the NOC DMA to a specific X/Y coord
	 */
	tt_bh_dma_api_config config;
};

DEVICE_API_EXTENDS(tt_bh_dma_noc, dma, dma_api);

static inline int tt_dma_config(const struct device *dev, uint32_t channel, struct dma_config *cfg,
				struct tt_bh_dma_noc_coords *coords)
{
	const struct tt_bh_dma_noc_driver_api *api = DEVICE_API_GET(tt_bh_dma_noc, dev);

	if (!api->config) {
		return -ENOSYS;
	}

	if (cfg == NULL || coords == NULL) {
		return -EINVAL;
	}

	return api->config(dev, channel, cfg, coords);
}
