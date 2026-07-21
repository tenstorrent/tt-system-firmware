/*
 * Copyright (c) 2022 Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Verify zephyr dma memory to memory transfer loops with scatter gather
 * @details
 * - Test Steps
 *   -# Set dma configuration for scatter gather enable
 *   -# Set direction memory-to-memory with two block transfers
 *   -# Start transfer tx -> rx
 * - Expected Results
 *   -# Data is transferred correctly from src buffers to dest buffers without
 *      software intervention.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/ztest.h>

#define XFERS        4
#define NUM_CHANNELS CONFIG_DMA_NUM_CHANNELS

#if CONFIG_NOCACHE_MEMORY
static __aligned(32) uint8_t tx_data[CONFIG_DMA_SG_XFER_SIZE] __used
	__attribute__((__section__(".nocache")));
static __aligned(32) uint8_t rx_data[XFERS][CONFIG_DMA_SG_XFER_SIZE] __used
	__attribute__((__section__(".nocache.dma")));
#else
/* this src memory shall be in RAM to support using as a DMA source pointer.*/
static __aligned(CONFIG_DMA_SG_ALIGNMENT) uint8_t tx_data[CONFIG_DMA_SG_XFER_SIZE];
static __aligned(CONFIG_DMA_SG_ALIGNMENT) uint8_t rx_data[XFERS][CONFIG_DMA_SG_XFER_SIZE] = {{0}};
#endif

static struct dma_config dma_cfg = {0};
static struct dma_block_config dma_block_cfgs[XFERS];
static struct k_sem xfer_sem;

static void dma_sg_callback(const struct device *dma_dev, void *user_data, uint32_t channel,
			    int status)
{
	if (status >= 0) {
		k_sem_give(&xfer_sem);
	}
}

static int test_sg(void)
{
	const struct device *dma;

	TC_PRINT("DMA memory to memory transfer started\n");
	TC_PRINT("Preparing DMA Controller\n");

	memset(tx_data, 0, sizeof(tx_data));

	for (int i = 0; i < CONFIG_DMA_SG_XFER_SIZE; i++) {
		tx_data[i] = i;
	}

	dma = DEVICE_DT_GET(DT_NODELABEL(tst_dma0));
	if (!device_is_ready(dma)) {
		TC_PRINT("dma controller device is not ready\n");
		return TC_FAIL;
	}

	dma_cfg.channel_direction = MEMORY_TO_MEMORY;
	dma_cfg.source_data_size = 1U;
	dma_cfg.dest_data_size = 1U;
	dma_cfg.source_burst_length = 1U;
	dma_cfg.dest_burst_length = 1U;
#ifdef CONFIG_DMAMUX_STM32
	dma_cfg.user_data = (struct device *)dma;
#else
	dma_cfg.user_data = NULL;
#endif /* CONFIG_DMAMUX_STM32 */
	dma_cfg.dma_callback = dma_sg_callback;
	dma_cfg.block_count = XFERS;
	dma_cfg.head_block = dma_block_cfgs;
	dma_cfg.complete_callback_en = false;

#ifdef CONFIG_DMA_MCUX_TEST_SLOT_START
	dma_cfg.dma_slot = CONFIG_DMA_MCUX_TEST_SLOT_START;
#endif

	/* Run the scatter-gather test on each channel one by one, assuming
	 * channels 0..NUM_CHANNELS-1 are available for use without requesting.
	 */
	for (int i = 0; i < NUM_CHANNELS; i++) {

		memset(rx_data, 0, sizeof(rx_data));
		memset(dma_block_cfgs, 0, sizeof(dma_block_cfgs));
		k_sem_init(&xfer_sem, 0, 1);

		for (int j = 0; j < XFERS; j++) {
			dma_block_cfgs[j].source_gather_en = 1U;
			dma_block_cfgs[j].block_size = CONFIG_DMA_SG_XFER_SIZE;
#ifdef CONFIG_DMA_64BIT
			dma_block_cfgs[j].source_address = (uint64_t)(tx_data);
			dma_block_cfgs[j].dest_address = (uint64_t)(rx_data[j]);
#else
			dma_block_cfgs[j].source_address = (uint32_t)(tx_data);
			dma_block_cfgs[j].dest_address = (uint32_t)(rx_data[j]);
#endif
			if (j < XFERS - 1) {
				dma_block_cfgs[j].next_block = &dma_block_cfgs[j + 1];
			}
		}

		TC_PRINT("Configuring the scatter-gather transfer on channel %d\n", i);

		if (dma_config(dma, i, &dma_cfg)) {
			TC_PRINT("ERROR: transfer config (%d)\n", i);
			return TC_FAIL;
		}

		if (dma_start(dma, i)) {
			TC_PRINT("ERROR: transfer start (%d)\n", i);
			return TC_FAIL;
		}

		if (k_sem_take(&xfer_sem, K_SECONDS(5)) != 0) {
			TC_PRINT("Timed out waiting for completion on channel %d\n", i);
			dma_stop(dma, i);
			return TC_FAIL;
		}

		TC_PRINT("Verify RX buffer should contain the full TX buffer string.\n");

		for (int j = 0; j < XFERS; j++) {
			if (memcmp(tx_data, rx_data[j], CONFIG_DMA_SG_XFER_SIZE)) {
				dma_stop(dma, i);
				return TC_FAIL;
			}
		}

		dma_stop(dma, i);
		k_msleep(10);
	}

	TC_PRINT("Finished: DMA Scatter-Gather\n");
	return TC_PASS;
}

/* export test cases */
ZTEST(dma_m2m_sg, test_dma_m2m_sg)
{
	zassert_true((test_sg() == TC_PASS));
}
