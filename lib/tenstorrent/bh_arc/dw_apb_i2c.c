/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/drivers/i2c.h>
#include "timer.h"
#include "dw_apb_i2c.h"
#include "asic_state.h"
#include "reg.h"
#include "util.h"

/* Pad and reset-unit register addresses used only for bus recovery bitbang. */
#define DW_APB_I2C_REG_MAP_BASE_ADDR      0x80060000
#define DW_APB_I2C1_REG_MAP_BASE_ADDR     0x80090000
#define DW_APB_I2C2_REG_MAP_BASE_ADDR     0x800A0000
#define RESET_UNIT_I2C_PAD_CNTL_REG_ADDR  0x800301C0
#define RESET_UNIT_I2C1_PAD_CNTL_REG_ADDR 0x800305CC
#define RESET_UNIT_I2C2_PAD_CNTL_REG_ADDR 0x800305D8
#define RESET_UNIT_I2C_PAD_DATA_REG_ADDR  0x800301C4
#define RESET_UNIT_I2C1_PAD_DATA_REG_ADDR 0x800305D0
#define RESET_UNIT_I2C2_PAD_DATA_REG_ADDR 0x800305DC
#define RESET_UNIT_I2C_CNTL_REG_ADDR      0x800300F0

#define DW_APB_I2C_IC_ENABLE_REG_OFFSET 0x0000006C

#define RESET_UNIT_I2C_PAD_CTRL_TRIEN_SCL_MASK 0x1
#define RESET_UNIT_I2C_PAD_CTRL_TRIEN_SDA_MASK 0x2
#define RESET_UNIT_I2C_PAD_CNTL_RXEN_MASK      0xC0
#define RESET_UNIT_I2C_PAD_CNTL_TRIEN_MASK     0x3
#define RESET_UNIT_I2C_PAD_CNTL_DRV_SHIFT      10

/* Custom abort source codes retained for backward compatibility with callers. */
#define IC_ABRT_A3_STATE (0x1 << 21)
#define IC_VERIFY_FAIL   (0x1 << 22)

#define NUM_I2C_CONTROLLERS 3

static const struct device *const i2c_devs[NUM_I2C_CONTROLLERS] = {
	DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c0)),
	DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c1)),
	DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c2)),
};

/* Target (slave) address for each I2C master bus, set via I2CInit. */
static uint16_t i2c_target_addr[NUM_I2C_CONTROLLERS];

extern uint8_t asic_state;

static const struct device *GetI2CDev(uint32_t id)
{
	if (id >= NUM_I2C_CONTROLLERS) {
		return NULL;
	}
	return i2c_devs[id];
}

static uint32_t GetI2CBaseAddress(uint32_t id)
{
	switch (id) {
	case 0:
		return DW_APB_I2C_REG_MAP_BASE_ADDR;
	case 1:
		return DW_APB_I2C1_REG_MAP_BASE_ADDR;
	case 2:
		return DW_APB_I2C2_REG_MAP_BASE_ADDR;
	default:
		return 0;
	}
}

/* Get I2C_PAD_CNTL register address with respect to RESET_UNIT. */
static uint32_t GetI2CPadCntlAddr(uint32_t id)
{
	switch (id) {
	case 0:
		return RESET_UNIT_I2C_PAD_CNTL_REG_ADDR;
	case 1:
		return RESET_UNIT_I2C1_PAD_CNTL_REG_ADDR;
	case 2:
		return RESET_UNIT_I2C2_PAD_CNTL_REG_ADDR;
	default:
		return 0;
	}
}

/* Get I2C_PAD_DATA register address with respect to RESET_UNIT. */
static uint32_t GetI2CPadDataAddr(uint32_t id)
{
	switch (id) {
	case 0:
		return RESET_UNIT_I2C_PAD_DATA_REG_ADDR;
	case 1:
		return RESET_UNIT_I2C1_PAD_DATA_REG_ADDR;
	case 2:
		return RESET_UNIT_I2C2_PAD_DATA_REG_ADDR;
	default:
		return 0;
	}
}

bool IsValidI2CMasterId(uint32_t id)
{
	const struct device *dev = GetI2CDev(id);

	return dev != NULL && device_is_ready(dev);
}

void I2CInitGPIO(uint32_t id)
{
	/* Pad configuration is handled by the Zephyr pinctrl driver at device init. */
	ARG_UNUSED(id);
}

/* Set the target device address for subsequent transactions on the given bus. */
void I2CInit(I2CMode mode, uint32_t slave_addr, I2CSpeedMode speed, uint32_t id)
{
	ARG_UNUSED(mode);
	ARG_UNUSED(speed);

	if (asic_state == A3State) {
		return;
	}
	if (id >= NUM_I2C_CONTROLLERS) {
		return;
	}
	i2c_target_addr[id] = (uint16_t)(slave_addr & 0x3FFU);
}

uint32_t I2CReadRxFifo(uint32_t id, uint8_t *p_read_buf)
{
	/* Not applicable to the Zephyr I2C API; reads are handled via i2c_transfer. */
	ARG_UNUSED(id);
	ARG_UNUSED(p_read_buf);
	return -ENOTSUP;
}

/* Generalized transaction: write write_len bytes then read read_len bytes. */
uint32_t I2CTransaction(uint32_t id, const uint8_t *write_data, uint32_t write_len,
			uint8_t *read_data, uint32_t read_len)
{
	if (asic_state == A3State) {
		return IC_ABRT_A3_STATE;
	}

	const struct device *dev = GetI2CDev(id);

	if (dev == NULL) {
		return -ENODEV;
	}

	uint16_t addr = i2c_target_addr[id];

	if (write_len > 0 && read_len > 0) {
		return i2c_write_read(dev, addr, write_data, write_len, read_data, read_len);
	}
	if (write_len > 0) {
		return i2c_write(dev, write_data, write_len, addr);
	}
	if (read_len > 0) {
		return i2c_read(dev, read_data, read_len, addr);
	}
	return 0;
}

uint32_t I2CWriteBytes(uint32_t id, uint16_t command, uint32_t command_byte_size,
		       const uint8_t *p_write_buf, uint32_t data_byte_size)
{
	if (asic_state == A3State) {
		return IC_ABRT_A3_STATE;
	}

	const struct device *dev = GetI2CDev(id);

	if (dev == NULL) {
		return -ENODEV;
	}

	uint32_t data_len = (p_write_buf != NULL) ? data_byte_size : 0;
	uint32_t total = command_byte_size + data_len;
	const uint8_t *cmd_buf = (const uint8_t *)&command;
	struct i2c_msg msgs[2];
	uint8_t num_msgs = 0;

	if (total == 0) {
		return 0;
	}

	if (command_byte_size > 0) {
		msgs[num_msgs].buf = (uint8_t *)cmd_buf;
		msgs[num_msgs].len = command_byte_size;
		msgs[num_msgs].flags = I2C_MSG_WRITE;
		num_msgs++;
	}

	if (data_len > 0) {
		msgs[num_msgs].buf = (uint8_t *)p_write_buf;
		msgs[num_msgs].len = data_len;
		msgs[num_msgs].flags = I2C_MSG_WRITE;
		num_msgs++;
	}

	msgs[num_msgs - 1].flags |= I2C_MSG_STOP;

	return i2c_transfer(dev, msgs, num_msgs, i2c_target_addr[id]);
}

uint32_t I2CReadBytes(uint32_t id, uint16_t command, uint32_t command_byte_size,
		      uint8_t *p_read_buf, uint32_t data_byte_size, uint8_t flip_bytes)
{
	uint32_t ic_error = I2CTransaction(id, (uint8_t *)&command, command_byte_size, p_read_buf,
					   data_byte_size);

	if (!ic_error && flip_bytes) {
		FlipBytes(p_read_buf, data_byte_size);
	}
	return ic_error;
}

uint32_t I2CRMWV(uint32_t id, uint16_t command, uint32_t command_byte_size, const uint8_t *p_data,
		 const uint8_t *p_mask, uint32_t data_byte_size)
{
	uint32_t ic_error;
	uint8_t buffer[data_byte_size];

	/* Read */
	ic_error = I2CReadBytes(id, command, command_byte_size, buffer, data_byte_size, 0);
	if (ic_error) {
		return ic_error;
	}

	/* Modify */
	for (uint32_t i = 0; i < data_byte_size; i++) {
		buffer[i] = (buffer[i] & ~p_mask[i]) | (p_data[i] & p_mask[i]);
	}

	/* Write */
	ic_error = I2CWriteBytes(id, command, command_byte_size, buffer, data_byte_size);
	if (ic_error) {
		return ic_error;
	}

	/* Verify */
	ic_error = I2CReadBytes(id, command, command_byte_size, buffer, data_byte_size, 0);
	if (ic_error) {
		return ic_error;
	}

	for (uint32_t i = 0; i < data_byte_size; i++) {
		if ((buffer[i] & p_mask[i]) != (p_data[i] & p_mask[i])) {
			return IC_VERIFY_FAIL;
		}
	}

	return 0;
}

/* Bitbang recovery sequence on I2C bus. */
void I2CRecoverBus(uint32_t id)
{
	uint32_t drive_strength = 0x7F; /* 50% of max 0xFF */
	uint32_t i2c_cntl = (drive_strength << RESET_UNIT_I2C_PAD_CNTL_DRV_SHIFT) |
			    RESET_UNIT_I2C_PAD_CNTL_TRIEN_MASK;
	uint32_t i2c_rst_cntl = ReadReg(RESET_UNIT_I2C_CNTL_REG_ADDR);

	/* Disable I2C controller */
	WriteReg(GetI2CBaseAddress(id) + DW_APB_I2C_IC_ENABLE_REG_OFFSET, 0);
	/* Release control of pads from I2C controller */
	WriteReg(RESET_UNIT_I2C_CNTL_REG_ADDR, i2c_rst_cntl & ~BIT(id));
	/* Init I2C pads for bitbang */
	WriteReg(GetI2CPadCntlAddr(id), i2c_cntl);
	/* Set both pads to output low */
	WriteReg(GetI2CPadDataAddr(id), 0x0);
	/*
	 * First, manually hold SCL low for 150 ms. Per the SMBUS spec,
	 * we should only need to hold the line low for 25 ms, but that does
	 * not work reliably and this does...
	 */
	i2c_cntl ^= RESET_UNIT_I2C_PAD_CTRL_TRIEN_SCL_MASK;
	WriteReg(GetI2CPadCntlAddr(id), i2c_cntl);
	Wait(150 * WAIT_1MS);
	/*
	 * Bitbang I2C reset to unstick bus. Hold SDA low, toggle SCL 32 times to create 16
	 * clock cycles. Note we toggle the TRIEN bit, as when TRIEN is
	 * set the bus will be released and external pullups will
	 * drive SCL high.
	 */
	for (int i = 0; i < 32; i++) {
		i2c_cntl ^= RESET_UNIT_I2C_PAD_CTRL_TRIEN_SCL_MASK;
		WriteReg(GetI2CPadCntlAddr(id), i2c_cntl);
		Wait(100 * WAIT_1US);
	}
	/* Add stop condition: transition SDA to high while SCL is high. */
	WriteReg(GetI2CPadCntlAddr(id), RESET_UNIT_I2C_PAD_CTRL_TRIEN_SCL_MASK);
	Wait(100 * WAIT_1US);
	WriteReg(GetI2CPadCntlAddr(id),
		 RESET_UNIT_I2C_PAD_CTRL_TRIEN_SCL_MASK | RESET_UNIT_I2C_PAD_CTRL_TRIEN_SDA_MASK);
	Wait(100 * WAIT_1US);
	/* Restore pads to input mode */
	WriteReg(GetI2CPadCntlAddr(id), (drive_strength << RESET_UNIT_I2C_PAD_CNTL_DRV_SHIFT) |
						RESET_UNIT_I2C_PAD_CNTL_RXEN_MASK |
						RESET_UNIT_I2C_PAD_CNTL_TRIEN_MASK);
	/* Return control of pads to I2C controller */
	WriteReg(RESET_UNIT_I2C_CNTL_REG_ADDR, i2c_rst_cntl | BIT(id));
	/* Re-enable I2C controller */
	WriteReg(GetI2CBaseAddress(id) + DW_APB_I2C_IC_ENABLE_REG_OFFSET, 1);
}
