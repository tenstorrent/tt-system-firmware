/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_TENSTORRENT_TT_BLACKHOLE_PINCTRL_SOC_H_
#define ZEPHYR_SOC_TENSTORRENT_TT_BLACKHOLE_PINCTRL_SOC_H_

#include <zephyr/devicetree.h>
#include <zephyr/types.h>
#include <zephyr/dt-bindings/pinctrl/tt_blackhole_smc-pinctrl.h>

typedef struct pinctrl_soc_pin {
	uint8_t pin;
	uint8_t af;
	uint8_t flags;
	uint8_t drive_strength;
} pinctrl_soc_pin_t;

#define TT_BH_DT_PIN_FLAGS(node_id)                                                                \
	((DT_PROP(node_id, bias_pull_up) ? PINCTRL_TT_BH_PUEN : 0) |                               \
	 (DT_PROP(node_id, bias_pull_down) ? PINCTRL_TT_BH_PDEN : 0) |                             \
	 (DT_PROP(node_id, input_enable) ? (PINCTRL_TT_BH_TRIEN | PINCTRL_TT_BH_RXEN) : 0) |       \
	 (DT_PROP(node_id, input_schmitt_enable) ? PINCTRL_TT_BH_STEN : 0))

#define TT_BH_DT_PIN(node_id)                                                                      \
	{                                                                                          \
		.pin = DT_PROP_BY_IDX(node_id, pinmux, 0),                                         \
		.af = DT_PROP_BY_IDX(node_id, pinmux, 1),                                          \
		.flags = TT_BH_DT_PIN_FLAGS(node_id),                                              \
		.drive_strength = DT_PROP_OR(node_id, drive_strength, PINCTRL_TT_BH_DRVS_DFLT),    \
	},

#define Z_PINCTRL_STATE_PIN_INIT(node_id, prop, idx)                                               \
	TT_BH_DT_PIN(DT_PROP_BY_IDX(node_id, prop, idx))

#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop)                                                   \
	{DT_FOREACH_PROP_ELEM(node_id, prop, Z_PINCTRL_STATE_PIN_INIT)}

#endif /* ZEPHYR_SOC_TENSTORRENT_TT_BLACKHOLE_PINCTRL_SOC_H_ */
