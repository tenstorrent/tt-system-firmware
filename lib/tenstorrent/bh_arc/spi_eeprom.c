/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "reg.h"
#include "status_reg.h"
#include "telemetry.h"
#include "util.h"

#include <stdbool.h>
#include <string.h>

#include <tenstorrent/board_variables.h>
#include <tenstorrent/smc_msg.h>
#include <tenstorrent/msgqueue.h>
#include <tenstorrent/sys_init_defines.h>
#include <tenstorrent/tt_boot_fs.h>
#include <zephyr/drivers/mspi.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#define SPI_PAGE_SIZE   256
#define SECTOR_SIZE     4096
#define SPI_BUFFER_SIZE 4096
#define BYTE_GET(v, b)  FIELD_GET(0xFFu << ((b) * 8), (v))

#define SSI_RX_DLY_SR_DEPTH            64
#define SPI_RX_SAMPLE_DELAY_TRAIN_ADDR 0x13FFC
#define SPI_RX_SAMPLE_DELAY_TRAIN_DATA 0xA5A55A5A

LOG_MODULE_REGISTER(spi_eeprom, CONFIG_TT_APP_LOG_LEVEL);

/* Temporary buffer to hold SPI page */
static uint8_t spi_page_buf[SPI_BUFFER_SIZE];
/* Global buffer for SPI programming */
static uint8_t spi_global_buffer[SPI_BUFFER_SIZE];
static struct flash_pages_info page_info;
static bool flash_locked = true;

static const struct device *flash = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi_flash));

static void EepromSetup(void)
{
	/* Setup SPI buffer address */
	WriteReg(RESET_UNIT_SCRATCH_RAM_REG_ADDR(10),
		 ((uint32_t)LOG2(SPI_BUFFER_SIZE) << 24) |
			 ((uint32_t)spi_global_buffer & 0xFFFFFF));
	/* Get flash device page size */
	flash_get_page_info_by_offs(flash, 0, &page_info);
}

static int SpiBlockRead(uint32_t spi_address, uint32_t num_bytes, uint8_t *dest)
{
	int rc;

	if (!device_is_ready(flash)) {
		/* Flash init failed */
		LOG_ERR("Flash device not ready");
		return -ENODEV;
	}
	rc = flash_read(flash, spi_address, dest, num_bytes);
	if (rc < 0) {
		LOG_ERR("%s failed %sat 0x%08x: %d", "Flash read", "", spi_address, rc);
	}
	return rc;
}

/* automatically erases sectors and merges incoming data with existing data as needed */
static int SpiSmartWrite(uint32_t address, const uint8_t *data, uint32_t num_bytes)
{
	uint32_t sector_size = page_info.size;
	uint32_t addr = ROUND_DOWN(address, sector_size);
	uint32_t chunk_size;
	int rc;

	__ASSERT(sector_size <= sizeof(spi_page_buf), "Sector size is larger than temp buffer");
	sys_trace_named_event("spiwrite", address, num_bytes);

	/* Phase 1: Write first chunk (may be unaligned)*/
	chunk_size = MIN((ROUND_UP((address + 1), sector_size) - address), num_bytes);
	rc = flash_read(flash, addr, spi_page_buf, sector_size);
	if (rc < 0) {
		LOG_ERR("%s failed %sat 0x%08x: %d", "Flash read", "[phase 1] ", addr, rc);
		return rc;
	}
	if (memcmp(&spi_page_buf[address - addr], data, chunk_size) != 0) {
		/* Write this block */
		memcpy(&spi_page_buf[address - addr], data, chunk_size);
		rc = flash_erase(flash, addr, sector_size);
		if (rc < 0) {
			LOG_ERR("%s failed %sat 0x%08x: %d", "Flash erase", "[phase 1] ", addr, rc);
			return rc;
		}
		rc = flash_write(flash, addr, spi_page_buf, sector_size);
		if (rc < 0) {
			LOG_ERR("%s failed %sat 0x%08x: %d", "Flash write", "[phase 1] ", addr, rc);
			return rc;
		}
	}
	addr += sector_size;
	data += chunk_size;
	num_bytes -= chunk_size;

	/* Phase 2: Write aligned data  */
	while (num_bytes > sector_size) {

		rc = flash_read(flash, addr, spi_page_buf, sector_size);
		if (rc < 0) {
			LOG_ERR("%s failed %sat 0x%08x: %d", "Flash read", "[phase 2] ", addr, rc);
			return rc;
		}
		if (memcmp(spi_page_buf, data, MIN(sector_size, sizeof(spi_page_buf))) != 0) {
			/* Write this block */
			rc = flash_erase(flash, addr, sector_size);
			if (rc < 0) {
				LOG_ERR("%s failed %sat 0x%08x: %d", "Flash erase", "[phase 2] ",
					addr, rc);
				return rc;
			}
			rc = flash_write(flash, addr, data, sector_size);
			if (rc < 0) {
				LOG_ERR("%s failed %sat 0x%08x: %d", "Flash write", "[phase 2] ",
					addr, rc);
				return rc;
			}
		}
		addr += sector_size;
		data += sector_size;
		num_bytes -= sector_size;
	}

	if (num_bytes == 0) {
		/* No need for phase 3 */
		return 0;
	}

	/* Phase 3: Write last chunk (may be unaligned) */
	chunk_size = num_bytes;
	rc = flash_read(flash, addr, spi_page_buf, sector_size);
	if (rc < 0) {
		LOG_ERR("%s failed %sat 0x%08x: %d", "Flash read", "[phase 3] ", addr, rc);
		return rc;
	}
	if (memcmp(spi_page_buf, data, chunk_size) != 0) {
		/* Write this block */
		memcpy(spi_page_buf, data, chunk_size);
		rc = flash_erase(flash, addr, sector_size);
		if (rc < 0) {
			LOG_ERR("%s failed %sat 0x%08x: %d", "Flash erase", "[phase 3] ", addr, rc);
			return rc;
		}
		rc = flash_write(flash, addr, spi_page_buf, sector_size);
		if (rc < 0) {
			LOG_ERR("%s failed %sat 0x%08x: %d", "Flash write", "[phase 3] ", addr, rc);
			return rc;
		}
	}
	return 0;
}

/* If we are using the spi buffer memory type, */
/* then make sure the passed in address and length is actually within the spi_buffer bounds. */
static bool check_csm_region(uint32_t addr, uint32_t num_bytes)
{
	return addr < (uint32_t)spi_global_buffer ||
	       (addr + num_bytes) > ((uint32_t)spi_global_buffer + sizeof(spi_global_buffer));
}

static uint8_t read_eeprom_handler(const union request *request, struct response *response)
{
	uint8_t buffer_mem_type = request->eeprom.buffer_mem_type;
	uint32_t spi_address = request->eeprom.spi_address;
	uint32_t num_bytes = request->eeprom.num_bytes;
	uint8_t *csm_addr = (uint8_t *)request->eeprom.csm_addr;

	if (!device_is_ready(flash)) {
		/* Flash init failed */
		return 1;
	}
	if (buffer_mem_type == 0) {
		/* Make sure that we are only interacting with our csm scratch buffer */
		if (check_csm_region((uint32_t)csm_addr, num_bytes)) {
			return 2;
		}
	} else {
		/* If we aren't reading from the csm; exit with error */
		return 1;
	}

	return SpiBlockRead(spi_address, num_bytes, csm_addr);
}

static uint8_t write_eeprom_handler(const union request *request, struct response *response)
{
	uint8_t buffer_mem_type = request->eeprom.buffer_mem_type;
	uint32_t spi_address = request->eeprom.spi_address;
	uint32_t num_bytes = request->eeprom.num_bytes;
	uint8_t *csm_addr = (uint8_t *)request->eeprom.csm_addr;

	if (flash_locked) {
		/* Flash is locked; cannot write */
		return 2;
	}

	if (!device_is_ready(flash)) {
		/* Flash init failed */
		return 1;
	}
	if (buffer_mem_type == 0) {
		/* Make sure that we are only interacting with our csm scratch buffer */
		if (check_csm_region((uint32_t)csm_addr, num_bytes)) {
			return 2;
		}
	} else {
		/* If we aren't reading from the csm; exit with error */
		return 1;
	}

	return SpiSmartWrite(spi_address, csm_addr, num_bytes);
}

/**
 * @brief Confirms SPI flash operation by echoing challenge data
 * @details Echoes the challenge data from request field back to response data[1]
 *          to confirm firmware update completion
 */
static uint8_t confirm_flashed_spi_handler(const union request *request, struct response *response)
{
	response->data[1] = request->confirm_flashed_spi.challenge_data;
	return 0;
}

/**
 * @brief Locks flash memory to prevent writes
 * @details Sets the global flash_locked flag to true, blocking all subsequent
 *          flash write operations until unlocked. The host must prove board
 *          compatibility again before the next unlock.
 */
static uint8_t flash_lock_handler(const union request *request, struct response *response)
{
	flash_locked = true;
	return 0;
}

/* Micron MT25QU512ABB, packed as telemetry reports it: 0x00MMTTCC. (Not the
 * same encoding as the bootrom SPI_DEVICE_ID register.)
 */
#define FLASH_JEDEC_ID_MT25QU512ABB 0x20BB20

/** @brief FLASH_UNLOCK exit code: the host did not verify every required
 * board variable. The required set is reported in response->data[1].
 */
#define FLASH_UNLOCK_ERR_MISSING_VARIABLES 1

/**
 * @brief The board variables a host must verify before this board may be flashed
 * @details See board_variables.h. Requiring a variable makes the board
 *          unflashable by any tool that does not know how to check it, so a
 *          variable is only required where getting it wrong would produce an
 *          unbootable image.
 */
static uint32_t required_board_variables(void)
{
	uint32_t required = 0;

	/* Exempt MT25 boards from the SPI flash check to allow old tt-flash
	 * & FW to succeed.
	 */
	if (GetTelemetryFlashJedecId() != FLASH_JEDEC_ID_MT25QU512ABB) {
		required |= BIT(BOARD_VAR_SPI_JEDEC_ID);
	}

	return required;
}

/**
 * @brief Unlocks flash memory to allow writes
 * @details Clears the global flash_locked flag, enabling flash write
 *          operations, but only once the host has declared that it verified
 *          every board variable this board requires.
 *
 *          An image is compatible with a board if its board type matches
 *          (always verified by SPI writers) and the board variable checks
 *          pass. A host that does not know a variable exists cannot have
 *          checked it, so it is refused rather than allowed to write an
 *          image that the board cannot boot.
 *
 *          We also need a special exception for tt-flash + luwen. luwen
 *          unlocks & relocks (with no variable verification bits) in every
 *          call to spi_flash, failing if the unlock fails. Thus, if already
 *          unlocked, we ignore a no-bit unlock.
 *
 *          The requirement is reported in response->data[1] on success and
 *          failure alike, so that a rejected host can name what it must
 *          check.
 */
static uint8_t flash_unlock_handler(const union request *request, struct response *response)
{
	uint32_t required = required_board_variables();
	uint32_t verified = 0;

	response->data[1] = required;

	if (!flash_locked && request->flash_unlock.num_variable_words == 0) {
		/* luwen spi_flash special case: tt-flash already did the real unlock */
		return 0;
	}

	if (request->flash_unlock.num_variable_words >= 1) {
		verified = request->flash_unlock.verified_variables[0];
	}

	bool flash_constraints_satisfied = ((required & ~verified) == 0);

	flash_locked = !flash_constraints_satisfied;

	return flash_constraints_satisfied ? 0 : FLASH_UNLOCK_ERR_MISSING_VARIABLES;
}

REGISTER_MESSAGE(TT_SMC_MSG_READ_EEPROM, read_eeprom_handler);
REGISTER_MESSAGE(TT_SMC_MSG_WRITE_EEPROM, write_eeprom_handler);
REGISTER_MESSAGE(TT_SMC_MSG_CONFIRM_FLASHED_SPI, confirm_flashed_spi_handler);
REGISTER_MESSAGE(TT_SMC_MSG_FLASH_LOCK, flash_lock_handler);
REGISTER_MESSAGE(TT_SMC_MSG_FLASH_UNLOCK, flash_unlock_handler);

static int InitSpiFS(void)
{
	if (!IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	EepromSetup();

	return 0;
}
SYS_INIT_APP(InitSpiFS);
