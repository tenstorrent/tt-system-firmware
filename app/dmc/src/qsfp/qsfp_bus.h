/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCU_I2C0 session and TCA9554 sideband for the four P150A cages.
 */

#ifndef APP_DMC_QSFP_BUS_H_
#define APP_DMC_QSFP_BUS_H_

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <tenstorrent/qsfp_mgmt.h>

#define TCA9554_REG_INPUT  0x00
#define TCA9554_REG_OUTPUT 0x01
#define TCA9554_REG_CONFIG 0x03

#define QSFP_BIT_MODPRSL BIT(0)
#define QSFP_BIT_RESETL  BIT(1)
#define QSFP_BIT_MODSELL BIT(2)
#define QSFP_BIT_LPMODE  BIT(3)
#define QSFP_BIT_INTL    BIT(4)

#define QSFP_CONFIG_DIR (QSFP_BIT_MODPRSL | QSFP_BIT_INTL | BIT(5) | BIT(6) | BIT(7))
#define QSFP_OUT_IDLE   0xFF

struct qsfp_cage {
	const char *name;
	uint8_t expander_addr;
};

extern const struct qsfp_cage qsfp_cages[QSFP_CAGE_COUNT];
extern uint8_t qsfp_output_shadow[QSFP_CAGE_COUNT];

const struct device *qsfp_session_begin(void);
void qsfp_session_end(const struct device *bus);
int qsfp_recover(const struct device *bus);
int qsfp_write_output(const struct device *bus, uint8_t cage, uint8_t value);
int qsfp_select(const struct device *bus, uint8_t cage);
void qsfp_deselect(const struct device *bus, uint8_t cage);

/* Capability cache in qsfp_mgmt.c; clear when a module leaves, is reset, or
 * may have been replaced (same SFF-8024 identifier is common across CMIS SKUs).
 */
void qsfp_dom_cache_invalidate(uint8_t cage);

#endif /* APP_DMC_QSFP_BUS_H_ */
