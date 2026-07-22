/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Test concurrent DMA transfers on all channels
 * @details
 * - Test Steps
 *   -# Set dma configuration for memory to memory transfer on 16 channels
 *   -# Start transfer on all channels concurrently and wait for callback to signal completion
 *   -# Check that TX and RX buffers match for each channel
 * - Expected Results
 *   -# Data is transferred correctly from src to dest
 */
#include <zephyr/drivers/dma.h>
#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

#define DMA_DATA_ALIGNMENT DT_PROP_OR(DT_NODELABEL(tst_dma0), dma_buf_addr_alignment, 32)
#define NUM_CHANNELS       DT_PROP(DT_NODELABEL(tst_dma0), dma_channels)
#define BLOCK_SIZE         256

static uint8_t tx_buf[NUM_CHANNELS][BLOCK_SIZE] __aligned(DMA_DATA_ALIGNMENT);
static uint8_t rx_buf[NUM_CHANNELS][BLOCK_SIZE] __aligned(DMA_DATA_ALIGNMENT);

static struct k_sem chan_sem[NUM_CHANNELS];

static void dma_callback(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	/* status non-zero means the server returned an error on this descriptor */
	zassert_equal(status, 0, "channel %d descriptor error", channel);
	k_sem_give(&chan_sem[channel]);
}

ZTEST(dma_blen_concurrent, test_all_channels_concurrently)
{
	const struct device *dma_dev = DEVICE_DT_GET(DT_NODELABEL(tst_dma0));
	struct dma_config cfg = {0};
	struct dma_block_config blk[NUM_CHANNELS] = {0};

	zassert_true(device_is_ready(dma_dev), "DMA device not ready");

	/* configure DMA settings */
	cfg.channel_direction = MEMORY_TO_MEMORY;
	cfg.source_data_size = 1U;
	cfg.dest_data_size = 1U;
	cfg.source_burst_length = 16;
	cfg.dest_burst_length = 8;
	cfg.dma_callback = dma_callback;
	cfg.complete_callback_en = 1;
	cfg.block_count = 1;

	/* set up each channel with something in their TX buf and clearing their RX buf*/
	for (int i = 0; i < NUM_CHANNELS; i++) {
		k_sem_init(&chan_sem[i], 0, 1);

		/* fill tx buf with unique pattern per channel */
		for (int j = 0; j < BLOCK_SIZE; j++) {
			tx_buf[i][j] = (uint8_t)(i ^ j);
		}
		memset(rx_buf[i], 0, BLOCK_SIZE);
	}

	/* configure all channels */
	for (int i = 0; i < NUM_CHANNELS; i++) {
		memset(&blk[i], 0, sizeof(blk[i]));
#ifdef CONFIG_DMA_64BIT
		blk[i].source_address = (uint64_t)tx_buf[i];
		blk[i].dest_address = (uint64_t)rx_buf[i];
#else
		blk[i].source_address = (uint32_t)tx_buf[i];
		blk[i].dest_address = (uint32_t)rx_buf[i];
#endif
		blk[i].block_size = BLOCK_SIZE;

		cfg.head_block = &blk[i];

		zassert_ok(dma_config(dma_dev, i, &cfg), "dma_config failed on channel %d", i);
	}

	for (int i = 0; i < NUM_CHANNELS; i++) {
		zassert_ok(dma_start(dma_dev, i), "dma_start failed on channel %d", i);
	}

	/* wait for all channels to report completion via callback */
	for (int i = 0; i < NUM_CHANNELS; i++) {
		int ret = k_sem_take(&chan_sem[i], K_MSEC(1000));

		zassert_ok(ret, "channel %d timed out (server may have stalled)", i);
	}

	/* check correctness between TX and RX buffers*/
	for (int i = 0; i < NUM_CHANNELS; i++) {
		zassert_mem_equal(rx_buf[i], tx_buf[i], BLOCK_SIZE, "data mismatch on channel %d",
				  i);
	}
}
