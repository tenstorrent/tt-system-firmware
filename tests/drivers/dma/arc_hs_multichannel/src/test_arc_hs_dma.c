/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Targeted ARC HS DMA coverage for multichannel ISR and multi-group descriptors.
 *
 * Exercises the interrupt path with:
 * - All 16 channels active concurrently (completion callbacks on each channel).
 * - A chained transfer with enough blocks that the completion handle is >= 32,
 *   requiring the ISR to process DONESTATD_AUX groups beyond group 0.
 */

#include <string.h>

#include <zephyr/drivers/dma.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#define NUM_CHANNELS CONFIG_DMA_NUM_CHANNELS
#define BLOCK_SIZE   CONFIG_DMA_BLOCK_SIZE
#define MULTI_BLOCKS CONFIG_DMA_MULTIGROUP_BLOCK_COUNT

static uint8_t tx_buf[NUM_CHANNELS][BLOCK_SIZE] __aligned(CONFIG_DMA_BLEN_ALIGNMENT);
static uint8_t rx_buf[NUM_CHANNELS][BLOCK_SIZE] __aligned(CONFIG_DMA_BLEN_ALIGNMENT);

static struct k_sem chan_sem[NUM_CHANNELS];

static uint8_t mg_tx[MULTI_BLOCKS][BLOCK_SIZE] __aligned(CONFIG_DMA_BLEN_ALIGNMENT);
static uint8_t mg_rx[MULTI_BLOCKS][BLOCK_SIZE] __aligned(CONFIG_DMA_BLEN_ALIGNMENT);
static struct dma_block_config mg_blocks[MULTI_BLOCKS];
static struct k_sem mg_done_sem;

static void concurrent_callback(const struct device *dev, void *user_data, uint32_t channel,
				int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	zassert_equal(status, 0, "channel %u descriptor error", channel);
	k_sem_give(&chan_sem[channel]);
}

static void multigroup_callback(const struct device *dev, void *user_data, uint32_t channel,
				int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	zassert_equal(status, 0, "multigroup transfer error on channel %u", channel);
	k_sem_give(&mg_done_sem);
}

ZTEST(dma_arc_hs_multichannel, test_all_channels_concurrent_interrupts)
{
	const struct device *dma_dev = DEVICE_DT_GET(DT_NODELABEL(dma0));
	struct dma_config cfg = {0};
	struct dma_block_config blk[NUM_CHANNELS] = {0};

	zassert_true(device_is_ready(dma_dev), "DMA device not ready");

	cfg.channel_direction = MEMORY_TO_MEMORY;
	cfg.source_data_size = 1U;
	cfg.dest_data_size = 1U;
	cfg.source_burst_length = 16;
	cfg.dest_burst_length = 8;
	cfg.dma_callback = concurrent_callback;
	cfg.complete_callback_en = 1;
	cfg.block_count = 1;

	for (int i = 0; i < NUM_CHANNELS; i++) {
		k_sem_init(&chan_sem[i], 0, 1);

		for (int j = 0; j < BLOCK_SIZE; j++) {
			tx_buf[i][j] = (uint8_t)(i ^ j);
		}
		memset(rx_buf[i], 0, BLOCK_SIZE);
	}

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

	for (int i = 0; i < NUM_CHANNELS; i++) {
		zassert_ok(k_sem_take(&chan_sem[i], K_SECONDS(5)),
			   "channel %d timed out waiting for ISR completion", i);
	}

	for (int i = 0; i < NUM_CHANNELS; i++) {
		zassert_mem_equal(rx_buf[i], tx_buf[i], BLOCK_SIZE, "data mismatch on channel %d",
				  i);
	}
}

ZTEST(dma_arc_hs_multichannel, test_multigroup_descriptor_interrupt)
{
	const struct device *dma_dev = DEVICE_DT_GET(DT_NODELABEL(dma0));
	struct dma_config cfg = {0};

	zassert_true(device_is_ready(dma_dev), "DMA device not ready");
	zassert_true(MULTI_BLOCKS > 32, "need >32 blocks to reach descriptor group 1");

	k_sem_init(&mg_done_sem, 0, 1);

	for (int i = 0; i < MULTI_BLOCKS; i++) {
		for (int j = 0; j < BLOCK_SIZE; j++) {
			mg_tx[i][j] = (uint8_t)(i + j);
		}
		memset(mg_rx[i], 0, BLOCK_SIZE);

		memset(&mg_blocks[i], 0, sizeof(mg_blocks[i]));
#ifdef CONFIG_DMA_64BIT
		mg_blocks[i].source_address = (uint64_t)mg_tx[i];
		mg_blocks[i].dest_address = (uint64_t)mg_rx[i];
#else
		mg_blocks[i].source_address = (uint32_t)mg_tx[i];
		mg_blocks[i].dest_address = (uint32_t)mg_rx[i];
#endif
		mg_blocks[i].block_size = BLOCK_SIZE;
		mg_blocks[i].next_block = (i < (MULTI_BLOCKS - 1)) ? &mg_blocks[i + 1] : NULL;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.channel_direction = MEMORY_TO_MEMORY;
	cfg.source_data_size = 1U;
	cfg.dest_data_size = 1U;
	cfg.source_burst_length = 1;
	cfg.dest_burst_length = 1;
	cfg.dma_callback = multigroup_callback;
	cfg.complete_callback_en = 1;
	cfg.block_count = MULTI_BLOCKS;
	cfg.head_block = &mg_blocks[0];

	zassert_ok(dma_config(dma_dev, 0, &cfg), "dma_config failed");
	zassert_ok(dma_start(dma_dev, 0), "dma_start failed");

	zassert_ok(k_sem_take(&mg_done_sem, K_SECONDS(10)),
		   "timed out waiting for handle >= 32 completion via ISR");

	for (int i = 0; i < MULTI_BLOCKS; i++) {
		zassert_mem_equal(mg_rx[i], mg_tx[i], BLOCK_SIZE, "data mismatch in block %d", i);
	}
}
