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

/*
 * Set per-channel NOC coordinates for the TT BH NOC DMA driver.
 */
int tt_bh_dma_noc_set_coords(const struct device *dev, uint32_t channel, uint8_t source_x,
			     uint8_t source_y, uint8_t dest_x, uint8_t dest_y);
