/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/init.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/mspi.h>
#include <zephyr/drivers/mspi/mspi_dw.h>

#define SPI_RX_TRAIN_ADDR   0x13FFC
#define SPI_RX_TRAIN_DATA   0xa5a55a5a
#define SSI_RX_DLY_SR_DEPTH 64

const struct device *mspi_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi0));
const struct device *flash = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi_flash));

/*
 * On boards with second-source flash support, spi_flash is a mux over
 * per-chip candidate nodes; bus position and max frequency are common to
 * all of them. On the other boards it is the flash chip node itself.
 */
#if DT_NODE_HAS_COMPAT(DT_NODELABEL(spi_flash), tenstorrent_flash_mux)
#define FLASH_CHIP_NODE DT_PHANDLE_BY_IDX(DT_NODELABEL(spi_flash), flash_devices, 0)
#else
#define FLASH_CHIP_NODE DT_NODELABEL(spi_flash)
#endif

/* SPI operating modes, defined by blackhole bootrom */
typedef enum {
	SpiStandardMode = 0,
	SpiDualMode = 1,
	SpiQuadMode = 2,
	SpiOctalMode = 3,
} SpiIoMode;

/* Reset unit SPI configuration register, indicates mode of SPI device. */
typedef struct {
	uint32_t boot_spi_mode: 2;
	uint32_t boot_ddr: 1;
	uint32_t boot_dqs: 1;
	uint32_t boot_address_mode: 4;
	uint32_t normal_spi_mode: 2;
	uint32_t normal_ddr: 1;
	uint32_t normal_dqs: 1;
	uint32_t normal_address_mode: 4;
	uint32_t device_addr_bytes: 4;
	uint32_t flash_family: 2;
} RESET_UNIT_SPI_DEVICE_CONFIG_reg_t;

typedef union {
	uint32_t val;
	RESET_UNIT_SPI_DEVICE_CONFIG_reg_t f;
} RESET_UNIT_SPI_DEVICE_CONFIG_reg_u;

#define RESET_UNIT_SPI_DEVICE_CONFIG_REG_ADDR 0x800300D4
#define RESET_UNIT_SPI_DEVICE_ID_REG_ADDR     0x800300D8

static int flash_reset_init(void)
{
	RESET_UNIT_SPI_DEVICE_CONFIG_reg_u spi_device_config;
	uint32_t spi_device_id;
	int rc;
	struct mspi_xfer_packet packet = {
		.dir = MSPI_TX,
	};
	struct mspi_xfer xfer = {
		.xfer_mode = MSPI_PIO,
		.packets = &packet,
		.num_packet = 1,
		.timeout = 10, /* 10 MSEC transfer timeout */
		.cmd_length = 1,
	};
	struct mspi_dev_cfg mspi_dev_cfg = {
		.freq = DT_PROP(FLASH_CHIP_NODE, mspi_max_frequency),
		.endian = MSPI_XFER_BIG_ENDIAN,
		.cmd_length = 1,
	};
	const struct mspi_dev_id mspi_dev_id = {
		.dev_idx = DT_REG_ADDR(FLASH_CHIP_NODE),
	};

	if (!device_is_ready(mspi_dev)) {
		return -ENODEV;
	}

	/* First, check the operation mode of the SPI controller */
	spi_device_config.val = sys_read32(RESET_UNIT_SPI_DEVICE_CONFIG_REG_ADDR);
	spi_device_id = sys_read32(RESET_UNIT_SPI_DEVICE_ID_REG_ADDR);

	mspi_dev_cfg.addr_length = spi_device_config.f.normal_address_mode ? 4 : 3;
	xfer.addr_length = 0;

	if ((spi_device_config.f.normal_spi_mode == SpiOctalMode) &&
	    (spi_device_id == 0x2c5b1a10)) {
		if (spi_device_config.f.normal_ddr) {
			/* MX35 flash in DDR mode. */
			mspi_dev_cfg.data_rate = MSPI_DATA_RATE_DUAL;
		}
		/* MX35 flash. Issue RESET ENABLE and RESET MEMORY commands in octal mode */
		mspi_dev_cfg.io_mode = MSPI_IO_MODE_OCTAL;
		rc = mspi_dev_config(mspi_dev, &mspi_dev_id, MSPI_DEVICE_CONFIG_ALL, &mspi_dev_cfg);
		if (rc < 0) {
			return rc;
		}
		/* Send RESET ENABLE */
		packet.cmd = 0x66;
		rc = mspi_transceive(mspi_dev, &mspi_dev_id, &xfer);
		if (rc < 0) {
			return rc;
		}
		/* Delay a minimum of 30 ns before sending next command */
		k_usleep(1);
		/* Send RESET MEMORY */
		packet.cmd = 0x99;
		rc = mspi_transceive(mspi_dev, &mspi_dev_id, &xfer);
		if (rc < 0) {
			return rc;
		}
		/* Delay a minimum of 40 ns to allow reset to complete */
		k_usleep(1);
		/* Write to SPI_DEVICE_CONFIG with new settings */
		spi_device_config.f.normal_spi_mode = SpiStandardMode;
		spi_device_config.f.normal_ddr = 0;
		spi_device_config.f.normal_address_mode = 3;
		spi_device_config.f.device_addr_bytes = 3;
		sys_write32(spi_device_config.val, RESET_UNIT_SPI_DEVICE_CONFIG_REG_ADDR);
	} else if ((spi_device_config.f.normal_spi_mode == SpiQuadMode) &&
		   (spi_device_id == 0x20bb2010)) {
		if (spi_device_config.f.normal_ddr) {
			/* MT25 flash in DDR mode. */
			mspi_dev_cfg.data_rate = MSPI_DATA_RATE_DUAL;
		}
		/* MT25 flash. Issue RESET ENABLE and RESET MEMORY commands in quad mode */
		mspi_dev_cfg.io_mode = MSPI_IO_MODE_QUAD;
		rc = mspi_dev_config(mspi_dev, &mspi_dev_id, MSPI_DEVICE_CONFIG_ALL, &mspi_dev_cfg);
		if (rc < 0) {
			return rc;
		}
		/* Send RESET ENABLE */
		packet.cmd = 0x66;
		rc = mspi_transceive(mspi_dev, &mspi_dev_id, &xfer);
		if (rc < 0) {
			return rc;
		}
		/* Delay a minimum of 30 ns before sending next command */
		k_usleep(1);
		/* Send RESET MEMORY */
		packet.cmd = 0x99;
		rc = mspi_transceive(mspi_dev, &mspi_dev_id, &xfer);
		if (rc < 0) {
			return rc;
		}
		/* Delay a minimum of 40 ns to allow reset to complete */
		k_usleep(1);
		spi_device_config.f.normal_spi_mode = SpiStandardMode;
		spi_device_config.f.normal_ddr = 0;
		spi_device_config.f.normal_address_mode = 3;
		spi_device_config.f.device_addr_bytes = 3;
		sys_write32(spi_device_config.val, RESET_UNIT_SPI_DEVICE_CONFIG_REG_ADDR);
	}

	/* Release the MSPI controller - it was acquired by the call to
	 * mspi_dev_config()
	 */
	(void)mspi_get_channel_status(mspi_dev, 0);

	return 0;
}

static int flash_training_init(void)
{
	struct mspi_dw_timing_cfg timing_cfg;
	/* To avoid false positive */
	uint32_t spi_rx_buf = 0xDEADBEEF;
	int rc;
	int delay_lb, delay_ub;

	if ((!device_is_ready(flash)) || (!device_is_ready(mspi_dev))) {
		return -ENODEV;
	}

	timing_cfg.rx_sample_dly = 0;

	/*
	 * Perform flash training here. We need to train the RX sample delay
	 * to be sure we have valid reads at higher frequencies
	 */

	/* First, find the lower delay setting that works */
	do {
		rc = mspi_timing_config(mspi_dev, NULL, MSPI_DW_RX_TIMING_CFG, (void *)&timing_cfg);
		if (rc < 0) {
			return rc;
		}
		rc = flash_read(flash, SPI_RX_TRAIN_ADDR, &spi_rx_buf, sizeof(spi_rx_buf));
		if (rc < 0) {
			return rc;
		}
		timing_cfg.rx_sample_dly++;
	} while ((spi_rx_buf != SPI_RX_TRAIN_DATA) &&
		 (timing_cfg.rx_sample_dly < SSI_RX_DLY_SR_DEPTH));
	delay_lb = timing_cfg.rx_sample_dly - 1;
	/* Find the upper bound on the delay setting */
	do {
		rc = mspi_timing_config(mspi_dev, NULL, MSPI_DW_RX_TIMING_CFG, (void *)&timing_cfg);
		if (rc < 0) {
			return rc;
		}
		rc = flash_read(flash, SPI_RX_TRAIN_ADDR, &spi_rx_buf, sizeof(spi_rx_buf));
		if (rc < 0) {
			return rc;
		}
		timing_cfg.rx_sample_dly++;
	} while ((spi_rx_buf == SPI_RX_TRAIN_DATA) &&
		 (timing_cfg.rx_sample_dly < SSI_RX_DLY_SR_DEPTH));
	delay_ub = timing_cfg.rx_sample_dly - 2;

	/* Find midpoint of both delay settings */
	timing_cfg.rx_sample_dly = (delay_ub - delay_lb) / 2 + delay_lb;
	return mspi_timing_config(mspi_dev, NULL, MSPI_DW_RX_TIMING_CFG, (void *)&timing_cfg);
}

static int flash_training_pre_reclock(void)
{
	return flash_training_init();
}

static int flash_training_post_reclock(void)
{
	return flash_training_init();
}

BUILD_ASSERT(CONFIG_FLASH_INIT_PRIORITY > CONFIG_FLASH_RESET_PRIORITY,
	     "The flash driver probes after the bootrom reset handoff");
#ifdef CONFIG_FLASH_TT_MUX
BUILD_ASSERT(CONFIG_FLASH_TT_MUX_INIT_PRIORITY < CONFIG_FLASH_TRAINING_PRIORITY,
	     "Flash training reads through the selected flash configuration");
#ifdef CONFIG_BH_FWTABLE
BUILD_ASSERT(CONFIG_FLASH_TT_MUX_INIT_PRIORITY < CONFIG_BH_FWTABLE_INIT_PRIORITY,
	     "bh_fwtable reads flash; the flash configuration must be selected first");
#endif
#endif

SYS_INIT(flash_reset_init, POST_KERNEL, CONFIG_FLASH_RESET_PRIORITY);
SYS_INIT(flash_training_pre_reclock, POST_KERNEL, CONFIG_FLASH_TRAINING_PRIORITY);
SYS_INIT(flash_training_post_reclock, APPLICATION, CONFIG_FLASH_TRAINING_PRIORITY);
