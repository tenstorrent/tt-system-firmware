/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DW_APB_I2C_H
#define DW_APB_I2C_H

#include <stdint.h>
#include <zephyr/drivers/i2c.h>

#define I2C_WRITE_BIT 0
#define I2C_READ_BIT  1

typedef enum {
	I2CMst = 0,
	I2CSlv = 1,
} I2CMode;

typedef enum {
	I2CStandardMode = 1,
	I2CFastMode = 2,
} I2CSpeedMode;

bool IsValidI2CMasterId(uint32_t id);
void I2CInitGPIO(uint32_t id);
void I2CInit(I2CMode mode, uint32_t slave_addr, I2CSpeedMode speed, uint32_t id);
uint32_t I2CReadRxFifo(uint32_t id, uint8_t *p_read_buf);
uint32_t I2CTransaction(uint32_t id, const uint8_t *write_data, uint32_t write_len,
			uint8_t *read_data, uint32_t read_len);
uint32_t I2CWriteBytes(uint32_t id, uint16_t command, uint32_t command_byte_size,
		       const uint8_t *p_write_buf, uint32_t data_byte_size);
uint32_t I2CReadBytes(uint32_t id, uint16_t command, uint32_t command_byte_size,
		      uint8_t *p_read_buf, uint32_t data_byte_size, uint8_t flip_bytes);
uint32_t I2CRMWV(uint32_t id, uint16_t command, uint32_t command_byte_size, const uint8_t *p_data,
		 const uint8_t *p_mask, uint32_t data_byte_size);
void I2CRecoverBus(uint32_t id);
#endif
