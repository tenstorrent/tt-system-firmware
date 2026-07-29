/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/arch/common/sys_bitops.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/dt-bindings/pinctrl/tt_blackhole_smc-pinctrl.h>

#define DW_APB_I2C0_REG_MAP_BASE_ADDR     0x80060000u
#define DW_APB_I2C1_REG_MAP_BASE_ADDR     0x80090000u
#define DW_APB_I2C2_REG_MAP_BASE_ADDR     0x800A0000u
#define RESET_UNIT_I2C_PAD_CNTL_REG_ADDR  0x800301C0u
#define RESET_UNIT_I2C1_PAD_CNTL_REG_ADDR 0x800305CCu
#define RESET_UNIT_I2C2_PAD_CNTL_REG_ADDR 0x800305D8u
#define RESET_UNIT_I2C_PAD_DATA_REG_ADDR  0x800301C4u
#define RESET_UNIT_I2C1_PAD_DATA_REG_ADDR 0x800305D0u
#define RESET_UNIT_I2C2_PAD_DATA_REG_ADDR 0x800305DCu
#define RESET_UNIT_I2C_CNTL_REG_ADDR      0x800300F0u

#define RESET_UNIT_I2C_PAD_CTRL_TRIEN_SCL_MASK 0x1u
#define RESET_UNIT_I2C_PAD_CTRL_TRIEN_SDA_MASK 0x2u
#define RESET_UNIT_I2C_PAD_CNTL_PUEN_MASK      0xCu
#define RESET_UNIT_I2C_PAD_CNTL_PDEN_MASK      0x30u
#define RESET_UNIT_I2C_PAD_CNTL_RXEN_MASK      0xC0u
#define RESET_UNIT_I2C_PAD_CNTL_STEN_MASK      0x300u
#define RESET_UNIT_I2C_PAD_CNTL_DRV_SHIFT      10u

static int i2c_id_from_reg(uintptr_t reg)
{
	switch (reg) {
	case DW_APB_I2C0_REG_MAP_BASE_ADDR:
		return 0;
	case DW_APB_I2C1_REG_MAP_BASE_ADDR:
		return 1;
	case DW_APB_I2C2_REG_MAP_BASE_ADDR:
		return 2;
	default:
		return -ENOTSUP;
	}
}

static uintptr_t i2c_pad_cntl_addr(uint8_t id)
{
	switch (id) {
	case 0:
		return RESET_UNIT_I2C_PAD_CNTL_REG_ADDR;
	case 1:
		return RESET_UNIT_I2C1_PAD_CNTL_REG_ADDR;
	case 2:
		return RESET_UNIT_I2C2_PAD_CNTL_REG_ADDR;
	default:
		return 0u;
	}
}

static uintptr_t i2c_pad_data_addr(uint8_t id)
{
	switch (id) {
	case 0:
		return RESET_UNIT_I2C_PAD_DATA_REG_ADDR;
	case 1:
		return RESET_UNIT_I2C1_PAD_DATA_REG_ADDR;
	case 2:
		return RESET_UNIT_I2C2_PAD_DATA_REG_ADDR;
	default:
		return 0u;
	}
}

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt, uintptr_t reg)
{
	int id;
	uint32_t pad_cfg;
	uint32_t drive_strength = PINCTRL_TT_BH_DRVS_DFLT;

	/* Non-I2C or empty pinctrl states are intentionally no-op for BH. */
	if ((pins == NULL) || (pin_cnt == 0u) || (reg == PINCTRL_REG_NONE)) {
		return 0;
	}

	id = i2c_id_from_reg(reg);

	if (id < 0) {
		return 0;
	}

	pad_cfg = 0u;

	for (uint8_t i = 0; i < pin_cnt; ++i) {
		drive_strength = pins[i].drive_strength & PINCTRL_TT_BH_DRVS_MAX;

		if (pins[i].flags & PINCTRL_TT_BH_TRIEN) {
			pad_cfg |= RESET_UNIT_I2C_PAD_CTRL_TRIEN_SCL_MASK |
				   RESET_UNIT_I2C_PAD_CTRL_TRIEN_SDA_MASK;
		}

		if (pins[i].flags & PINCTRL_TT_BH_PUEN) {
			pad_cfg |= RESET_UNIT_I2C_PAD_CNTL_PUEN_MASK;
		}

		if (pins[i].flags & PINCTRL_TT_BH_PDEN) {
			pad_cfg |= RESET_UNIT_I2C_PAD_CNTL_PDEN_MASK;
		}

		if (pins[i].flags & PINCTRL_TT_BH_RXEN) {
			pad_cfg |= RESET_UNIT_I2C_PAD_CNTL_RXEN_MASK;
		}

		if (pins[i].flags & PINCTRL_TT_BH_STEN) {
			pad_cfg |= RESET_UNIT_I2C_PAD_CNTL_STEN_MASK;
		}
	}

	pad_cfg |= (drive_strength << RESET_UNIT_I2C_PAD_CNTL_DRV_SHIFT);

	sys_write32(pad_cfg, i2c_pad_cntl_addr((uint8_t)id));
	sys_write32(0u, i2c_pad_data_addr((uint8_t)id));
	sys_set_bit(RESET_UNIT_I2C_CNTL_REG_ADDR, (uint32_t)id);

	return 0;
}
