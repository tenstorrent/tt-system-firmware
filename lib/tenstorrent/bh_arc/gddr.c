/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bh_reset.h"
#include "gddr.h"
#include "harvesting.h"
#include "init.h"
#include "noc.h"
#include "noc_init.h"
#include "noc2axi.h"
#include "reg.h"
#include "status_reg.h"

#include <tenstorrent/bh_power.h>
#include <tenstorrent/msgqueue.h>
#include <tenstorrent/post_code.h>
#include <tenstorrent/smc_msg.h>
#include <tenstorrent/spi_flash_buf.h>
#include <tenstorrent/sys_init_defines.h>
#include <tenstorrent/tt_boot_fs.h>
#include <zephyr/drivers/misc/bh_fwtable.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control/clock_control_tt_bh.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/dma_tt_bh_noc.h>
#include <zephyr/drivers/dma/dma_arc_hs.h>

static const struct device *const pll_dev_3 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(pll3));
static const struct device *flash = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi_flash));
#ifdef CONFIG_DMA_ARC_HS
static const struct device *const arc_dma_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(dma0));
#endif
static const struct device *dma_noc = DEVICE_DT_GET(DT_NODELABEL(dma1));

/* This is the noc2axi instance we want to run the MRISC FW on */
#define MRISC_FW_NOC2AXI_PORT 0
#define MRISC_SETUP_TLB       13
#define MRISC_L1_ADDR         (1ULL << 37)
#define MRISC_REG_ADDR        (1ULL << 40)
#define MRISC_FW_CFG_OFFSET   0x3C00
#define ARC_NOC0_X            8
#define ARC_NOC0_Y            0
#define MRISC_L1_SIZE         (128 * 1024)

#define MRISC_FW_TAG     "memfw"
#define MRISC_FW_CFG_TAG "memfwcfg"

LOG_MODULE_REGISTER(gddr, CONFIG_TT_APP_LOG_LEVEL);

static const struct device *const fwtable_dev = DEVICE_DT_GET(DT_NODELABEL(fwtable));

static struct gddr_bist_info gddr_bist;

struct gddr_bist_info get_gddr_bist_info(void)
{
	return gddr_bist;
}

static uint32_t GetGddrSpeedFromCfg(uint8_t *fw_cfg_image)
{
	/* GDDR speed is the second DWORD of the MRISC FW Config table */
	uint32_t *fw_cfg_dw = (uint32_t *)fw_cfg_image;
	return fw_cfg_dw[1];
}

static volatile void *SetupMriscL1Tlb(uint8_t gddr_inst)
{
	uint8_t x, y;

	GetGddrNocCoords(gddr_inst, MRISC_FW_NOC2AXI_PORT, 0, &x, &y);
	NOC2AXITlbSetup(0, MRISC_SETUP_TLB, x, y, MRISC_L1_ADDR);
	return GetTlbWindowAddr(0, MRISC_SETUP_TLB, MRISC_L1_ADDR);
}

static uint32_t MriscL1Read32(uint8_t gddr_inst, uint32_t addr)
{
	uint8_t x, y;

	GetGddrNocCoords(gddr_inst, MRISC_FW_NOC2AXI_PORT, 0, &x, &y);
	NOC2AXITlbSetup(0, MRISC_SETUP_TLB, x, y, MRISC_L1_ADDR);
	return NOC2AXIRead32(0, MRISC_SETUP_TLB, MRISC_L1_ADDR + addr);
}

static void MriscL1Write32(uint8_t gddr_inst, uint32_t addr, uint32_t val)
{
	uint8_t x, y;

	GetGddrNocCoords(gddr_inst, MRISC_FW_NOC2AXI_PORT, 0, &x, &y);
	NOC2AXITlbSetup(0, MRISC_SETUP_TLB, x, y, MRISC_L1_ADDR);
	NOC2AXIWrite32(0, MRISC_SETUP_TLB, MRISC_L1_ADDR + addr, val);
}

static uint32_t MriscRegRead32(uint8_t gddr_inst, uint32_t addr)
{
	uint8_t x, y;

	GetGddrNocCoords(gddr_inst, MRISC_FW_NOC2AXI_PORT, 0, &x, &y);
	NOC2AXITlbSetup(0, MRISC_SETUP_TLB, x, y, MRISC_REG_ADDR + addr);
	return NOC2AXIRead32(0, MRISC_SETUP_TLB, MRISC_REG_ADDR + addr);
}

static void MriscRegWrite32(uint8_t gddr_inst, uint32_t addr, uint32_t val)
{
	uint8_t x, y;

	GetGddrNocCoords(gddr_inst, MRISC_FW_NOC2AXI_PORT, 0, &x, &y);
	NOC2AXITlbSetup(0, MRISC_SETUP_TLB, x, y, MRISC_REG_ADDR + addr);
	NOC2AXIWrite32(0, MRISC_SETUP_TLB, MRISC_REG_ADDR + addr, val);
}

int read_gddr_telemetry_table(uint8_t gddr_inst, gddr_telemetry_table_t *gddr_telemetry)
{
#ifdef CONFIG_DMA_ARC_HS
	volatile uint8_t *mrisc_l1 = SetupMriscL1Tlb(gddr_inst);
	bool dma_ok = false;

#ifdef CONFIG_DMA_ARC_HS
	dma_ok = dma_arc_hs_transfer(arc_dma_dev, 0,
				     (const void *)(mrisc_l1 + GDDR_TELEMETRY_TABLE_ADDR),
				     gddr_telemetry, sizeof(*gddr_telemetry), K_MSEC(500)) >= 0;
#endif
	if (!dma_ok) {
		for (int i = 0; i < sizeof(*gddr_telemetry) / 4; i++) {
			((uint32_t *)gddr_telemetry)[i] =
				MriscL1Read32(gddr_inst, GDDR_TELEMETRY_TABLE_ADDR + i * 4);
		}
	}
#endif
	/* Check that version matches expectation. */
	if (gddr_telemetry->telemetry_table_version != GDDR_TELEMETRY_TABLE_T_VERSION) {
		LOG_WRN_ONCE("GDDR telemetry table version mismatch: %d (expected %d)",
			     gddr_telemetry->telemetry_table_version,
			     GDDR_TELEMETRY_TABLE_T_VERSION);
		return -ENOTSUP;
	}
	return 0;
}

static void ReleaseMriscReset(uint8_t gddr_inst)
{
	const uint32_t kSoftReset0Addr = 0xFFB121B0;
	uint8_t x, y;

	GetGddrNocCoords(gddr_inst, MRISC_FW_NOC2AXI_PORT, 0, &x, &y);
	NOC2AXITlbSetup(0, MRISC_SETUP_TLB, x, y, kSoftReset0Addr);

	volatile uint32_t *soft_reset_0 = GetTlbWindowAddr(0, MRISC_SETUP_TLB, kSoftReset0Addr);
	*soft_reset_0 &= ~(1 << 11); /* Clear bit corresponding to MRISC reset */
}

static void SetAxiEnable(uint8_t gddr_inst, uint8_t noc2axi_port, bool axi_enable)
{
	const uint32_t kNiuCfg0Addr[NUM_NOCS] = {0xFFB20100, 0xFFB30100};
	uint8_t x, y;
	volatile uint32_t *niu_cfg_0[NUM_NOCS];

	for (uint8_t i = 0; i < NUM_NOCS; i++) {
		GetGddrNocCoords(gddr_inst, noc2axi_port, i, &x, &y);
		/* Note this actually sets up two TLBs (one for each NOC) */
		NOC2AXITlbSetup(i, MRISC_SETUP_TLB, x, y, kNiuCfg0Addr[i]);
		niu_cfg_0[i] = GetTlbWindowAddr(i, MRISC_SETUP_TLB, kNiuCfg0Addr[i]);
	}

	if (axi_enable) {
		for (uint8_t i = 0; i < NUM_NOCS; i++) {
			*niu_cfg_0[i] |= (1 << NIU_CFG_0_AXI_SLAVE_ENABLE);
		}
	} else {
		for (uint8_t i = 0; i < NUM_NOCS; i++) {
			*niu_cfg_0[i] &= ~(1 << NIU_CFG_0_AXI_SLAVE_ENABLE);
		}
	}
}

static int LoadMriscFw(uint8_t gddr_inst, uint8_t *buf, size_t buf_size, size_t spi_address,
		       size_t image_size)
{
	volatile uint32_t *mrisc_l1 = SetupMriscL1Tlb(gddr_inst);
	int rc = spi_arc_dma_transfer_to_tile(flash, spi_address, image_size, buf, buf_size,
					      (uint8_t *)mrisc_l1);

	return rc;
}
static int LoadMriscFwCfg(uint8_t gddr_inst, uint8_t *buf, size_t buf_size, size_t spi_address,
			  size_t image_size)
{
	volatile uint32_t *mrisc_l1 = SetupMriscL1Tlb(gddr_inst);
	int rc = spi_arc_dma_transfer_to_tile(flash, spi_address, image_size, buf, buf_size,
					      (uint8_t *)mrisc_l1 + MRISC_FW_CFG_OFFSET);

	if (rc == 0) {
		/* Set the controller_id field to the GDDR instance */
		volatile gddr_params_table_t *params_table =
			(volatile gddr_params_table_t *)((uint8_t *)mrisc_l1 + MRISC_FW_CFG_OFFSET);
		if (params_table->params_table_version >= 6) {
			params_table->controller_id = gddr_inst;
		} else {
			LOG_WRN_ONCE("MRISC params table version %d does not support controller_id "
				     "field (>= 6 required)",
				     params_table->params_table_version);
		}
	}

	return rc;
}

static uint32_t GetDramMask(void)
{
	uint32_t dram_mask = tile_enable.gddr_enabled; /* bit mask */

	if (tt_bh_fwtable_get_fw_table(fwtable_dev)->has_dram_table &&
	    tt_bh_fwtable_get_fw_table(fwtable_dev)->dram_table.dram_mask_en) {
		dram_mask &= tt_bh_fwtable_get_fw_table(fwtable_dev)->dram_table.dram_mask;
	}
	return dram_mask;
}

static int check_mrisc_busy(uint8_t gddr_inst)
{
	uint32_t status = MriscRegRead32(gddr_inst, MRISC_MSG_REGISTER);

	if (status != MRISC_MSG_TYPE_NONE) {
		LOG_WRN("GDDR %d message buffer is not free. Current value: 0x%x", gddr_inst,
			status);
		return -EBUSY;
	}
	return 0;
}

static int wait_mrisc_not_busy(uint8_t gddr_inst, k_timepoint_t timeout, const char *op_desc)
{
	while (MriscRegRead32(gddr_inst, MRISC_MSG_REGISTER) != 0) {
		/* Wait for the message to be processed */
		if (sys_timepoint_expired(timeout)) {
			LOG_ERR("Timeout after %d ms waiting for GDDR instance %d to run %s",
				MRISC_MEMTEST_TIMEOUT, gddr_inst, op_desc);
			return -ETIMEDOUT;
		}
	}
	return 0;
}

static int StartHwMemtest(uint8_t gddr_inst, uint32_t addr_bits, uint32_t start_addr, uint32_t mask)
{
	uint32_t msg_args[3] = {addr_bits, start_addr, mask};

	/* Only run if MRISC FW support it. Must be > 2.6 */
	gddr_telemetry_table_t gddr_telemetry;

	if (read_gddr_telemetry_table(gddr_inst, &gddr_telemetry) < 0) {
		LOG_WRN("Failed to read GDDR telemetry table while starting memtest");
		return -ENOTSUP;
	}
	if (gddr_telemetry.mrisc_fw_version_major < 2 ||
	    (gddr_telemetry.mrisc_fw_version_major == 2 &&
	     gddr_telemetry.mrisc_fw_version_minor < 7)) {
		LOG_WRN("GDDR %d MRISC FW version %d.%d does not support memtest", gddr_inst,
			gddr_telemetry.mrisc_fw_version_major,
			gddr_telemetry.mrisc_fw_version_minor);
		return -ENOTSUP;
	}

	/*
	 * Messaging should not be done concurrently to the same GDDR instance, but still do sanity
	 * check if the message buffer is free.
	 */
	int32_t ret = check_mrisc_busy(gddr_inst);

	if (ret != 0) {
		return ret;
	}

	if (addr_bits > 26) {
		LOG_WRN("Invalid number of address bits for memory test. Expected <= 26, got %d",
			addr_bits);
		return -EINVAL;
	}
	for (int i = 0; i < 3; i++) {
		MriscL1Write32(gddr_inst, GDDR_MSG_STRUCT_ADDR + i * 4, msg_args[i]);
	}
	MriscRegWrite32(gddr_inst, MRISC_MSG_REGISTER, MRISC_MSG_TYPE_RUN_MEMTEST);
	return 0;
}

static int CheckHwMemtestResult(uint8_t gddr_inst, k_timepoint_t timeout)
{
	/* This should only be called after StartHwMemtest() has already been called. */
	int32_t ret = wait_mrisc_not_busy(gddr_inst, timeout, "memtest");

	if (ret != 0) {
		gddr_bist.failed |= BIT(gddr_inst);
		return ret;
	}

	uint32_t pass = MriscL1Read32(gddr_inst, GDDR_MSG_STRUCT_ADDR + 8 * 4);

	gddr_bist.complete |= BIT(gddr_inst);
	if (pass != 0) {
		gddr_bist.failed |= BIT(gddr_inst);
		LOG_ERR("GDDR %d memory test failed", gddr_inst);
		return -EIO;
	}
	gddr_bist.failed &= ~BIT(gddr_inst);
	LOG_DBG("GDDR %d memory test passed", gddr_inst);
	return 0;
}

/* This function assumes that tensix L1s have already been cleared */
static void wipe_l1(void)
{
	uint8_t noc_id = 0;
	uint64_t addr = 0;
	uint32_t dram_mask = GetDramMask();
	uint8_t tensix_x, tensix_y;

	GetEnabledTensix(&tensix_x, &tensix_y);

	struct tt_bh_dma_noc_coords coords = tt_bh_dma_noc_coords_init(tensix_x, tensix_y, 0, 0);

	struct dma_block_config block = {
		.source_address = addr,
		.dest_address = addr,
		.block_size = MRISC_L1_SIZE,
	};

	struct dma_config config = {
		.channel_direction = PERIPHERAL_TO_MEMORY,
		.source_data_size = 1,
		.dest_data_size = 1,
		.source_burst_length = 1,
		.dest_burst_length = 1,
		.block_count = 1,
		.head_block = &block,
		.user_data = &coords,
	};

	for (uint32_t gddr_inst = 0; gddr_inst < NUM_GDDR; gddr_inst++) {
		if (IS_BIT_SET(dram_mask, gddr_inst)) {
			for (uint32_t noc2axi_port = 0; noc2axi_port < NUM_MRISC_NOC2AXI_PORT;
			     noc2axi_port++) {
				uint8_t x, y;

				GetGddrNocCoords(gddr_inst, noc2axi_port, noc_id, &x, &y);

				coords.dest_x = x;
				coords.dest_y = y;

				/* AXI enable must not be set, using MRISC address 0 */
				dma_config(dma_noc, 1, &config);
				dma_start(dma_noc, 1);
			}
		}
	}
}

static int InitMrisc(void)
{
	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEP9);

	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY) || !IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	/* In cable fault mode, GDDR tiles are clock-gated - skip MRISC init
	 * since tiles are not active.
	 */
	if (is_cable_fault_mode()) {
		return 0;
	}

	wipe_l1();

	/* Load MRISC (DRAM RISC) FW to all DRAMs in the middle NOC node */

	for (uint8_t gddr_inst = 0; gddr_inst < NUM_GDDR; gddr_inst++) {
		for (uint8_t noc2axi_port = 0; noc2axi_port < 3; noc2axi_port++) {
			SetAxiEnable(gddr_inst, noc2axi_port, true);
		}
	}

	uint32_t dram_mask = GetDramMask();

	int rc;
	tt_boot_fs_fd tag_fd;
	size_t image_size;
	size_t spi_address;

	uint8_t buf[SCRATCHPAD_SIZE] __aligned(4);

	rc = tt_boot_fs_find_fd_by_tag(flash, MRISC_FW_TAG, &tag_fd);
	if (rc < 0) {
		LOG_ERR("%s (%s) failed: %d", "tt_boot_fs_find_fd_by_tag", MRISC_FW_TAG, rc);
		record_init_failure(INIT_STAGE_MRISC_LOAD);
		return rc;
	}
	image_size = tag_fd.flags.f.image_size;
	spi_address = tag_fd.spi_addr;

	for (uint8_t gddr_inst = 0; gddr_inst < NUM_GDDR; gddr_inst++) {
		if (IS_BIT_SET(dram_mask, gddr_inst)) {
			if (LoadMriscFw(gddr_inst, buf, SCRATCHPAD_SIZE, spi_address, image_size)) {
				LOG_ERR("%s(%d) failed: %d", "LoadMriscFw", gddr_inst, -EIO);
				record_init_failure(INIT_STAGE_MRISC_LOAD);
				return -EIO;
			}
		}
	}

	rc = tt_boot_fs_find_fd_by_tag(flash, MRISC_FW_CFG_TAG, &tag_fd);
	if (rc < 0) {
		LOG_ERR("%s (%s) failed: %d", "tt_boot_fs_find_fd_by_tag", MRISC_FW_CFG_TAG, rc);
		record_init_failure(INIT_STAGE_MRISC_LOAD);
		return rc;
	}
	image_size = tag_fd.flags.f.image_size;
	spi_address = tag_fd.spi_addr;

	/* Loading ETH FW configuration data requires the whole data to be loaded into buffer */
	__ASSERT(SCRATCHPAD_SIZE >= image_size,
		 "spi buffer size %zu must be larger than image size %zu", SCRATCHPAD_SIZE,
		 image_size);

	rc = flash_read(flash, spi_address, buf, image_size);
	if (rc < 0) {
		LOG_ERR("%s() failed: %d", "flash_read", rc);
		record_init_failure(INIT_STAGE_MRISC_LOAD);
		return rc;
	}

	uint32_t gddr_speed = GetGddrSpeedFromCfg(buf);

	if (!IN_RANGE(gddr_speed, MIN_GDDR_SPEED, MAX_GDDR_SPEED)) {
		LOG_WRN("%s() failed: %d", "GetGddrSpeedFromCfg", gddr_speed);
		gddr_speed = MIN_GDDR_SPEED;
	}

	if (clock_control_set_rate(
		    pll_dev_3, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_GDDRMEMCLK,
		    (clock_control_subsys_rate_t)(gddr_speed / GDDR_SPEED_TO_MEMCLK_RATIO))) {
		LOG_ERR("%s(%d) failed: %d", "SetGddrMemClk", gddr_speed, -EIO);
		record_init_failure(INIT_STAGE_MRISC_LOAD);
		return -EIO;
	}

	for (uint8_t gddr_inst = 0; gddr_inst < NUM_GDDR; gddr_inst++) {
		if (IS_BIT_SET(dram_mask, gddr_inst)) {
			if (LoadMriscFwCfg(gddr_inst, buf, SCRATCHPAD_SIZE, spi_address,
					   image_size)) {
				LOG_ERR("%s(%d) failed: %d", "LoadMriscFwCfg", gddr_inst, -EIO);
				record_init_failure(INIT_STAGE_MRISC_LOAD);
				return -EIO;
			}
			MriscRegWrite32(gddr_inst, MRISC_INIT_STATUS, MRISC_INIT_BEFORE);
			ReleaseMriscReset(gddr_inst);
		}
	}

	return 0;
}
SYS_INIT_APP(InitMrisc);

static int CheckGddrTraining(uint8_t gddr_inst, k_timepoint_t timeout)
{
	do {
		uint32_t poll_val = MriscRegRead32(gddr_inst, MRISC_INIT_STATUS);

		if (poll_val == MRISC_INIT_FINISHED) {
			return 0;
		}
		if (poll_val == MRISC_INIT_FAILED) {
			LOG_ERR("%s[%d]: 0x%x", "MRISC_INIT_STATUS", gddr_inst, poll_val);
			return -EIO;
		}
		k_msleep(1);
	} while (!sys_timepoint_expired(timeout));

	LOG_ERR("%s[%d]: 0x%x", "MRISC_POST_CODE", gddr_inst,
		MriscRegRead32(gddr_inst, MRISC_POST_CODE));

	return -ETIMEDOUT;
}

static int CheckGddrHwTest(void)
{
	/* First kick off all tests in parallel, then check their results. Test will take
	 * approximately 300-400 ms.
	 */
	uint8_t test_started = 0; /* Bitmask of tests started */
	int any_error = 0;

	for (uint8_t gddr_inst = 0; gddr_inst < NUM_GDDR; gddr_inst++) {
		if (IS_BIT_SET(tile_enable.gddr_enabled, gddr_inst)) {
			int error = StartHwMemtest(gddr_inst, 26, 0, 0);

			if (error == -ENOTSUP) {
				/* Shouldn't be considered a test failure if MRISC FW is too old. */
				LOG_DBG("%s(%d) %s: %d", "StartHwMemtest", gddr_inst, "skipped",
					error);
			} else if (error < 0) {
				LOG_ERR("%s(%d) %s: %d", "StartHwMemtest", gddr_inst, "failed",
					error);
				any_error = -EIO;
			} else {
				test_started |= BIT(gddr_inst);
			}
		}
	}
	k_timepoint_t timeout = sys_timepoint_calc(K_MSEC(MRISC_MEMTEST_TIMEOUT));

	for (uint8_t gddr_inst = 0; gddr_inst < NUM_GDDR; gddr_inst++) {
		if (IS_BIT_SET(test_started, gddr_inst)) {
			int error = CheckHwMemtestResult(gddr_inst, timeout);

			if (error < 0) {
				any_error = -EIO;
				LOG_ERR("%s(%d) %s: %d", "CheckHwMemtestResult", gddr_inst,
					"failed", error);
			} else {
				LOG_DBG("%s(%d) %s: %d", "CheckHwMemtestResult", gddr_inst,
					"succeeded", error);
			}
		}
	}
	return any_error;
}

static int gddr_training(void)
{
	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEPE);

	/* Check GDDR training status. */
	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY) || !IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	/* In cable fault mode, GDDR tiles are clock-gated - skip training
	 * since tiles are not active.
	 */
	if (is_cable_fault_mode()) {
		return 0;
	}

	bool init_errors = false;
	k_timepoint_t timeout = sys_timepoint_calc(K_MSEC(MRISC_INIT_TIMEOUT));

	for (uint8_t gddr_inst = 0; gddr_inst < NUM_GDDR; gddr_inst++) {
		if (IS_BIT_SET(GetDramMask(), gddr_inst)) {
			int error = CheckGddrTraining(gddr_inst, timeout);

			if (error == -ETIMEDOUT) {
				LOG_ERR("GDDR instance %d timed out during training", gddr_inst);
				init_errors = true;
			} else if (error) {
				LOG_ERR("GDDR instance %d failed training", gddr_inst);
				init_errors = true;
			}
		}
	}

	if (!init_errors) {
		/* this is needed to securely wipe DRAM */
		if (CheckGddrHwTest() < 0) {
			LOG_ERR("GDDR HW test failed");
			record_init_failure(INIT_STAGE_GDDR_TRAIN);
			return -EIO;
		}
	} else {
		record_init_failure(INIT_STAGE_GDDR_TRAIN);
	}

	return 0;
}

static int32_t mrisc_message(uint32_t op_code, uint32_t instance_mask, uint32_t timeout_ms,
			     const char *op_desc)
{
	for (uint8_t gddr_inst = 0U; gddr_inst < NUM_GDDR; gddr_inst++) {
		if (IS_BIT_SET(instance_mask, gddr_inst)) {

			int ret = check_mrisc_busy(gddr_inst);

			if (ret != 0) {
				return ret;
			}
			MriscRegWrite32(gddr_inst, MRISC_MSG_REGISTER, op_code);
		}
	}
	k_timepoint_t timeout = sys_timepoint_calc(K_MSEC(timeout_ms));

	for (uint8_t gddr_inst = 0U; gddr_inst < NUM_GDDR; gddr_inst++) {
		if (IS_BIT_SET(instance_mask, gddr_inst)) {

			int ret = wait_mrisc_not_busy(gddr_inst, timeout, op_desc);

			if (ret != 0) {
				return ret;
			}
		}
	}

	return 0;
}

int32_t set_mrisc_power_setting(bool on)
{
	uint32_t op_code = on ? MRISC_MSG_TYPE_PHY_WAKEUP : MRISC_MSG_TYPE_PHY_POWERDOWN;

	return mrisc_message(op_code, GetDramMask(), MRISC_POWER_SETTING_TIMEOUT_MS,
			     "power_setting");
}

SYS_INIT_APP(gddr_training);

static void assert_mrisc_soft_reset(uint8_t gddr_inst)
{
	const uint32_t kSoftReset0Addr = 0xFFB121B0;
	const uint32_t kAllRiscSoftReset = 0x47800;

	for (uint8_t noc_node = 0; noc_node < NUM_MRISC_NOC2AXI_PORT; noc_node++) {
		uint8_t x, y;

		GetGddrNocCoords(gddr_inst, noc_node, 0, &x, &y);
		NOC2AXITlbSetup(0, MRISC_SETUP_TLB, x, y, kSoftReset0Addr);
		NOC2AXIWrite32(0, MRISC_SETUP_TLB, kSoftReset0Addr, kAllRiscSoftReset);
	}
}

/**
 * @brief Toggle GDDR MRISC reset, re-train, and re-run BIST
 *
 * Sequence:
 *   1) Assert MRISC soft reset
 *   2) Release MRISC reset
 *   3) Wait for GDDR training completion
 *   4) Re-run BIST
 *   5) If PHY was powered down, issue PHY wakeup
 */
static uint8_t toggle_gddr_reset(const union request *req, struct response *rsp)
{
	uint32_t gddr_inst = req->gddr_reset.gddr_inst;
	bool original_mrisc_state;
	int rc;

	bh_power_state_get(BH_POWER_DOMAIN_MRISC, &original_mrisc_state);

	if (gddr_inst >= NUM_GDDR) {
		rsp->data[1] = GDDR_RESET_ERR_INVALID_INST;
		return 1;
	}

	if (!IS_BIT_SET(tile_enable.gddr_enabled, gddr_inst)) {
		rsp->data[1] = GDDR_RESET_ERR_HARVESTED;
		return 1;
	}

	if (!IS_BIT_SET(GetDramMask(), gddr_inst)) {
		rsp->data[1] = GDDR_RESET_ERR_MASKED;
		return 1;
	}

	gddr_bist.complete &= ~BIT(gddr_inst);
	gddr_bist.failed &= ~BIT(gddr_inst);

	if (!original_mrisc_state) {
		rc = set_mrisc_power_setting(true);
		if (rc < 0) {
			rsp->data[1] = GDDR_RESET_ERR_POWERDOWN;
			return 1;
		}
	}

	assert_mrisc_soft_reset(gddr_inst);

	MriscRegWrite32(gddr_inst, MRISC_INIT_STATUS, MRISC_INIT_BEFORE);
	ReleaseMriscReset(gddr_inst);

	k_timepoint_t timeout = sys_timepoint_calc(K_MSEC(MRISC_INIT_TIMEOUT));

	rc = CheckGddrTraining(gddr_inst, timeout);
	if (rc < 0) {
		rsp->data[1] = GDDR_RESET_ERR_TRAINING;
		return 1;
	}

	rc = StartHwMemtest(gddr_inst, 26, 0, 0);
	if (rc < 0) {
		rsp->data[1] = GDDR_RESET_ERR_BIST;
		return 1;
	}

	timeout = sys_timepoint_calc(K_MSEC(MRISC_MEMTEST_TIMEOUT));
	rc = CheckHwMemtestResult(gddr_inst, timeout);
	if (rc < 0) {
		rsp->data[1] = GDDR_RESET_ERR_BIST;
		return 1;
	}

	if (!original_mrisc_state) {
		rc = set_mrisc_power_setting(false);
		if (rc < 0) {
			rsp->data[1] = GDDR_RESET_ERR_POWERDOWN;
			return 1;
		}
	}

	rsp->data[1] = 0;
	return 0;
}

#ifndef CONFIG_TT_SMC_RECOVERY
REGISTER_MESSAGE(TT_SMC_MSG_TOGGLE_GDDR_RESET, toggle_gddr_reset);
#endif
