/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef INCLUDE_ZEPHYR_DT_BINDINGS_PINCTRL_TT_BLACKHOLE_SMC_PINCTRL_H_
#define INCLUDE_ZEPHYR_DT_BINDINGS_PINCTRL_TT_BLACKHOLE_SMC_PINCTRL_H_

#define PINCTRL_TT_BH_AF0 0
#define PINCTRL_TT_BH_AF1 1

#define PINCTRL_TT_BH_TRIEN 0x01
#define PINCTRL_TT_BH_PUEN  0x02
#define PINCTRL_TT_BH_PDEN  0x04
#define PINCTRL_TT_BH_RXEN  0x08
#define PINCTRL_TT_BH_STEN  0x10

#define PINCTRL_TT_BH_DRVS_SHIFT 5
#define PINCTRL_TT_BH_DRVS_BITS  8
#define PINCTRL_TT_BH_DRVS_MAX   0xff
#define PINCTRL_TT_BH_DRVS_DFLT  0x7f

#define PINCTRL_TT_BH_DRVS(n) (((n) & PINCTRL_TT_BH_DRVS_MAX) << PINCTRL_TT_BH_DRVS_SHIFT)

#define TT_BH_PINMUX_A51_UART0_TX 48 PINCTRL_TT_BH_AF1(0)
#define TT_BH_PINMUX_B15_UART0_RX 49 PINCTRL_TT_BH_AF1(PINCTRL_TT_BH_TRIEN | PINCTRL_TT_BH_RXEN)

/**
 * @brief Configure a Blackhole pin for a non-gpio purpose
 *
 * e.g.
 * TT_BH_PINMUX(A, 51, UART0_TX)
 * TT_BH_PINMUX(B, 15, UART0_RX)
 *
 * @param col Column (letter) of BGA ball
 * @param row Row (number) of BGA ball
 * @param func Function of the pin e.g. UART0_TX
 */
#define TT_BH_PINMUX(col, row, func) TT_BH_PINMUX_##col##row##_##func

/**
 * @brief Configure a Blackhole pin as a GPIO
 *
 * @param num GPIO pin number
 * @param flags Additional GPIO flags, or zero.
 */
#define TT_BH_PINMUX_GPIO(num, flags) num TT_BH_PINCTL_AF0 flags

#endif
