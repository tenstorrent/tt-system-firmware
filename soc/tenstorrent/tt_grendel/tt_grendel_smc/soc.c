/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>

#include "platform.h"

#define UART_INIT_IF_OKAY(n, ...)                                                                  \
	do {                                                                                       \
		if (DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(uart##n))) {                              \
			uart_x_init(SMC_CPU_CTRL_LOCAL_BASE_REG_DEFAULT +                          \
				    SMC_CPU_UART_WRAP##n##_UART_CTRL_REG_ADDR);                    \
		}                                                                                  \
	} while (false)

#define I3C_INIT_IF_OKAY(n, ...)                                                                   \
	do {                                                                                       \
		if (DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(i3c##n))) {                               \
			i3c_x_init(SMC_CPU_CTRL_LOCAL_BASE_REG_DEFAULT +                           \
				   SMC_CPU_I3C_WRAP_##n##_I3C_CTRL_REG_MAP_BASE_ADDR);             \
		}                                                                                  \
	} while (false)

static void uart_x_init(uint32_t base_addr)
{
	UART_CTRL_reg_u uart_ctrl;

	uart_ctrl.val = sys_read32(base_addr);
	uart_ctrl.f.uart_en = 1;
	uart_ctrl.f.uart_reset_n_n0_scan = 1;
	sys_write32(uart_ctrl.val, base_addr);
}

static void i3c_x_init(uint32_t i3c_base_addr)
{
	I3C_CTRL_I3C_RESET_CTRL_STATUS_reg_u i3c_ctrl;
	I3C_CTRL_PINSTRAPS_GROUP_1A_reg_u i3c_pinstraps;

	/* Configure I3C role as controller */
	i3c_pinstraps.val = sys_read32(i3c_base_addr +
				       SMC_CPU_I3C_WRAP_0_I3C_CTRL_PINSTRAPS_GROUP_1A_REG_OFFSET);
	i3c_pinstraps.f.device_role = 0x0; /* Primary Controller */
	sys_write32(i3c_pinstraps.val,
		    i3c_base_addr + SMC_CPU_I3C_WRAP_0_I3C_CTRL_PINSTRAPS_GROUP_1A_REG_OFFSET);

	/* Enable I3Cx */
	i3c_ctrl.val = sys_read32(i3c_base_addr +
				  SMC_CPU_I3C_WRAP_0_I3C_CTRL_I3C_RESET_CTRL_STATUS_REG_OFFSET);
	i3c_ctrl.f.i3c_reset_n_n0_scan = 1;
	i3c_ctrl.f.reg_reset_n_n0_scan = 1;
	i3c_ctrl.f.i3c_enable_n0_scan = 1;
	sys_write32(i3c_ctrl.val,
		    i3c_base_addr + SMC_CPU_I3C_WRAP_0_I3C_CTRL_I3C_RESET_CTRL_STATUS_REG_OFFSET);
}

void uart_init(void)
{

	LISTIFY(4, UART_INIT_IF_OKAY, (;));
}

void i3c_init(void)
{
	LISTIFY(6, I3C_INIT_IF_OKAY, (;));
}

/**
 * @brief Early hardware init hook
 */
void soc_early_init_hook(void)
{
	SMC_WRAP_RESET_UNIT_MASTER_PERIPHERAL_RESETS_reg_u reset_reg;

	reset_reg.val = sys_read32(SMC_CPU_CTRL_LOCAL_BASE_REG_DEFAULT +
				   SMC_CPU_RESET_UNIT_PERIPHERAL_RESETS_REG_ADDR);

	/* Take peripherals out of reset */
	reset_reg.f.uart_reset_n_n0_scan = DT_HAS_COMPAT_STATUS_OKAY(ns16550);
	reset_reg.f.i3c_reset_n_n0_scan = DT_HAS_COMPAT_STATUS_OKAY(cdns_i3c);

	sys_write32(reset_reg.val, SMC_CPU_CTRL_LOCAL_BASE_REG_DEFAULT +
					   SMC_CPU_RESET_UNIT_PERIPHERAL_RESETS_REG_ADDR);

	uart_init();
	i3c_init();
}
