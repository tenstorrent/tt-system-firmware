/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "noc2axi.h"
#include "noc_init.h"
#include "harvesting.h"

#include <stdint.h>
#include <string.h>

#include <tenstorrent/post_code.h>
#include <tenstorrent/spi_flash_buf.h>
#include <tenstorrent/sys_init_defines.h>
#include <tenstorrent/tt_boot_fs.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/misc/bh_fwtable.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/dma_tt_bh_noc.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(tensix_init, CONFIG_TT_APP_LOG_LEVEL);

#define ARC_NOC0_X 8
#define ARC_NOC0_Y 0

#define TENSIX_X_START 2
#define TENSIX_Y_START 2
#define TENSIX_X_END   1
#define TENSIX_Y_END   11
#define TENSIX_L1_SIZE (1536 * 1024)

/* Tensix RISC control registers */
#define TRISC0_RESET_PC         0xFFB12228
#define TRISC_RESET_PC_OVERRIDE 0xFFB12234
#define SOFT_RESET_0            0xFFB121B0
#define ALL_RISC_SOFT_RESET     0x47800

/* TRISC0 wipe firmware parameters */
#define TRISC_WIPE_FW_TAG       "destwipe"
#define TRISC_WIPE_FW_LOAD_ADDR 0x6000 /* TRISC0_CODE region start */

/* Scratchpad buffer size for SPI transfers */
#define SCRATCHPAD_SIZE CONFIG_TT_BH_ARC_SCRATCHPAD_SIZE

/* Counter location for wipe_dest */
#define COUNTER_TENSIX_X     1
#define COUNTER_TENSIX_Y     2
#define COUNTER_L1_ADDR      0x110000 /* Must match firmware hardcoded value */
#define NUM_TENSIX_ROWS      10
#define WIPE_DEST_TIMEOUT_US 10000 /* 10ms timeout */

static const struct device *const fwtable_dev = DEVICE_DT_GET(DT_NODELABEL(fwtable));
static const struct device *const dma_noc = DEVICE_DT_GET(DT_NODELABEL(dma1));
static const struct device *const flash = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi_flash));

/* Enable CG_CTRL_EN in each non-harvested Tensix node and set CG hystersis to 2. */
/* This requires NOC init so that broadcast is set up properly. */

/* We enable CG for all blocks, but for reference the bit assignments are */
/* 0 - Register Blocks */
/* 1 - FPU */
/* 2 - FPU M Tile */
/* 3 - FPU SFPU */
/* 4 - Mover */
/* 5 - Packers */
/* 6 - Unpacker 0 */
/* 7 - Unpacker 1 */
/* 8 - X Search */
/* 9 - Thread Controller */
/* 10 - TRISC 0 */
/* 11 - TRISC 1 */
/* 12 - TRISC 2 */
/* 13 - L1 Return Muxes */
/* 14 - Instruction Thread */
/* 15 - L1 Banks */
/* 16 - Src B */

static void EnableTensixCG(void)
{
	uint8_t ring = 0;
	uint8_t noc_tlb = 0;

	/* CG hysteresis for the blocks. (Some share a field.) */
	/* Set them all to 2. */
	uint32_t cg_ctrl_hyst0 = 0xFFB12070;
	uint32_t cg_ctrl_hyst1 = 0xFFB12074;
	uint32_t cg_ctrl_hyst2 = 0xFFB1207C;

	uint32_t all_blocks_hyst_2 = 0x02020202;

	/* Enable CG for all blocks. */
	uint32_t cg_ctrl_en = 0xFFB12244;
	uint32_t enable_all_tensix_cg = 0xFFFFFFFF; /* Only bits 0-16 are used. */

	NOC2AXITensixBroadcastTlbSetup(ring, noc_tlb, cg_ctrl_en, kNoc2AxiOrderingStrict);

	NOC2AXIWrite32(ring, noc_tlb, cg_ctrl_hyst0, all_blocks_hyst_2);
	NOC2AXIWrite32(ring, noc_tlb, cg_ctrl_hyst1, all_blocks_hyst_2);
	NOC2AXIWrite32(ring, noc_tlb, cg_ctrl_hyst2, all_blocks_hyst_2);

	NOC2AXIWrite32(ring, noc_tlb, cg_ctrl_en, enable_all_tensix_cg);
}

/**
 * @brief Zeros the l1 of every non-harvested tensix core
 *
 * First zero the l1 of an arbitrary non-harvested tensix core, then broadcasts the zero'd l1 to
 * all other non-harvested tensix cores. This approach is faster than iterating over all tensix
 * cores sequentially to clear each l1.
 */
static void wipe_l1(void)
{
	uint64_t addr = 0;
	uint8_t tensix_x, tensix_y;
	/* NOC2AXI to Tensix L1 transactions must be aligned to 64 bytes */
	uint8_t sram_buffer[CONFIG_TT_BH_ARC_SCRATCHPAD_SIZE] __aligned(64);

	GetEnabledTensix(&tensix_x, &tensix_y);

	/* wipe SCRATCHPAD_SIZE of the chosen tensix */
	memset(sram_buffer, 0, sizeof(sram_buffer));

	struct tt_bh_dma_noc_coords coords =
		tt_bh_dma_noc_coords_init(tensix_x, tensix_y, ARC_NOC0_X, ARC_NOC0_Y);

	struct dma_block_config block = {
		.source_address = addr,
		.dest_address = (uintptr_t)sram_buffer,
		.block_size = sizeof(sram_buffer),
	};

	struct dma_config config = {
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.source_data_size = 1,
		.dest_data_size = 1,
		.source_burst_length = 1,
		.dest_burst_length = 1,
		.block_count = 1,
		.head_block = &block,
		.user_data = &coords,
	};

	dma_config(dma_noc, 1, &config);
	dma_start(dma_noc, 1);

	/* wipe entire L1 of the chosen tensix */
	uint32_t offset = sizeof(sram_buffer);

	while (offset < TENSIX_L1_SIZE) {
		uint32_t size = MIN(offset, TENSIX_L1_SIZE - offset);

		config.channel_direction = PERIPHERAL_TO_MEMORY;
		coords.dest_x = tensix_x;
		coords.dest_y = tensix_y;
		block.dest_address = offset;
		block.block_size = size;

		dma_config(dma_noc, 1, &config);
		dma_start(dma_noc, 1);

		offset += offset;
	}

	/* clear all remaining tensix L1 using the already-cleared L1 as a source */
	config.channel_direction = TT_BH_DMA_NOC_CHANNEL_DIRECTION_BROADCAST;
	block.source_address = addr;
	block.dest_address = addr;
	block.block_size = TENSIX_L1_SIZE;

	dma_config(dma_noc, 1, &config);
	dma_start(dma_noc, 1);
}

/**
 * @brief Global synchronization for wipe_dest
 *
 * This function is used to synchronize the wipe_dest operation across all tensix cores.
 * It reads the counter from the chosen tensix core and waits for it to reach the expected count.
 * It returns 0 if the counter reached the expected count, -ETIMEDOUT otherwise.
 */
static int global_sync(uint8_t ring, uint8_t noc_tlb, uint32_t expected_count)
{
	NOC2AXITlbSetup(ring, noc_tlb, COUNTER_TENSIX_X, COUNTER_TENSIX_Y, COUNTER_L1_ADDR);

	if (!WAIT_FOR(NOC2AXIRead32(ring, noc_tlb, COUNTER_L1_ADDR) >= expected_count,
		      WIPE_DEST_TIMEOUT_US, k_busy_wait(10))) {
		uint32_t actual = NOC2AXIRead32(ring, noc_tlb, COUNTER_L1_ADDR);

		LOG_ERR("%s: timeout, counter=%u expected=%u", __func__, actual, expected_count);
		return -ETIMEDOUT;
	}

	return 0;
}

/**
 * @brief Helper function to write 32-bit words to NOC
 *
 * This function is used to write 32-bit words to NOC using DMA.
 */
static int noc2axi_write32_fw(uint8_t *src, uint8_t *dst, size_t len)
{
	const uint32_t *fw_words = (const uint32_t *)src;
	size_t num_words = len / sizeof(uint32_t);
	uintptr_t addr = (uintptr_t)dst;

	for (size_t i = 0; i < num_words; i++) {
		NOC2AXIWrite32(0, 0, addr + i * sizeof(uint32_t), fw_words[i]);
	}
	return 0;
}

/**
 * @brief Setup the multicast TLB for the unharvested tensix cores
 *
 * @param addr The address to load the firmware to
 */
static inline void setup_tensix_mcast_tlb(uint32_t addr)
{
	uint8_t ring = 0;
	uint8_t noc_tlb = 0;

	NOC2AXIMulticastTlbSetup(ring, noc_tlb, TENSIX_X_START, TENSIX_Y_START, TENSIX_X_END,
				 TENSIX_Y_END, addr, kNoc2AxiOrderingStrict);
}

/**
 * @brief Zeros the DEST register of every non-harvested tensix core
 *
 * The DEST register can only be written by code running on the local TRISC.
 * This function loads a wipe firmware from SPI flash to each Tensix's L1,
 * runs it on TRISC 0 to clear DEST using 32-bit stores, then puts TRISC 0
 * back in reset.
 */
static int wipe_dest(void)
{
	uint8_t ring = 0;
	uint8_t noc_tlb = 0;
	uint8_t wipe_dest_buf[SCRATCHPAD_SIZE] __aligned(4);

	int rc;
	tt_boot_fs_fd tag_fd;
	size_t image_size;
	size_t spi_address;

	/* Find the TRISC wipe firmware in SPI flash */
	rc = tt_boot_fs_find_fd_by_tag(flash, (const uint8_t *)TRISC_WIPE_FW_TAG, &tag_fd);
	if (rc < 0) {
		LOG_ERR("%s(%s) failed: %d", "tt_boot_fs_find_fd_by_tag", TRISC_WIPE_FW_TAG, rc);
		return rc;
	}
	image_size = tag_fd.flags.f.image_size;
	spi_address = tag_fd.spi_addr;
	LOG_INF("%s: found %s at 0x%x, size %zu", __func__, TRISC_WIPE_FW_TAG, spi_address,
		image_size);

	/* Step 1: Zero the completion counter before releasing TRISCs */
	NOC2AXITlbSetup(ring, noc_tlb, COUNTER_TENSIX_X, COUNTER_TENSIX_Y, COUNTER_L1_ADDR);
	NOC2AXIWrite32(ring, noc_tlb, COUNTER_L1_ADDR, 0);

	/* Step 2: Load wipe firmware to all non-harvested Tensix L1 using multicast */
	setup_tensix_mcast_tlb(TRISC_WIPE_FW_LOAD_ADDR);

	/* Round up to ensure all 32-bit writes are complete */
	image_size = ROUND_UP(image_size, sizeof(uint32_t));

	rc = spi_transfer_by_parts(
		flash, spi_address, image_size, wipe_dest_buf, sizeof(wipe_dest_buf),
		(uint8_t *)(uintptr_t)TRISC_WIPE_FW_LOAD_ADDR, noc2axi_write32_fw);
	if (rc < 0) {
		LOG_ERR("%s(%s) failed: %d", "spi_transfer_by_parts", TRISC_WIPE_FW_TAG, rc);
		return rc;
	}
	LOG_INF("%s: firmware loaded", __func__);

	/* Step 3: Set TRISC 0 reset PC to firmware load address on all Tensix */
	setup_tensix_mcast_tlb(TRISC0_RESET_PC);
	NOC2AXIWrite32(ring, noc_tlb, TRISC0_RESET_PC, TRISC_WIPE_FW_LOAD_ADDR);
	NOC2AXIWrite32(ring, noc_tlb, TRISC_RESET_PC_OVERRIDE, 1);

	/* Step 4: Release TRISC 0 from soft reset on all Tensix */
	NOC2AXIWrite32(ring, noc_tlb, SOFT_RESET_0, ALL_RISC_SOFT_RESET & ~BIT(12));

	/* Step 5: Wait for all cores to signal completion via atomic counter */
	uint32_t expected = POPCOUNT(tile_enable.tensix_col_enabled) * NUM_TENSIX_ROWS;
	int rc_sync = global_sync(ring, noc_tlb, expected);

	/* Step 6: Re-assert TRISC 0 soft reset on all Tensix */
	setup_tensix_mcast_tlb(SOFT_RESET_0);
	NOC2AXIWrite32(ring, noc_tlb, SOFT_RESET_0, ALL_RISC_SOFT_RESET);
	NOC2AXIWrite32(ring, noc_tlb, TRISC_RESET_PC_OVERRIDE, 0);

	if (rc_sync < 0) {
		LOG_ERR("%s: global_sync failed: %d", __func__, rc_sync);
		return rc_sync;
	}

	LOG_INF("%s: completed", __func__);
	return 0;
}

void TensixInit(void)
{
	if (!tt_bh_fwtable_get_fw_table(fwtable_dev)->feature_enable.cg_en) {
		EnableTensixCG();
	}

	/* wipe_l1()/wipe_dest() aren't here because they're only needed on boot & board reset. */
}

static int tensix_init(void)
{
	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEPD);

	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY) || !IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	TensixInit();

	wipe_l1();
	int rc_wipe_dest = wipe_dest();

	if (rc_wipe_dest < 0) {
		LOG_ERR("%s: wipe_dest failed: %d", __func__, rc_wipe_dest);
		return rc_wipe_dest;
	}

	return 0;
}
SYS_INIT_APP(tensix_init);
