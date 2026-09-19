/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QSFP-DD bus ownership and TCA9554 sideband control on P150A.
 */

#include "qsfp.h"
#include "qsfp_bus.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(qsfp_bus, CONFIG_TT_APP_LOG_LEVEL);

const struct qsfp_cage qsfp_cages[QSFP_CAGE_COUNT] = {
	{"A", 0x38},
	{"B", 0x39},
	{"C", 0x3a},
	{"D", 0x3b},
};

uint8_t qsfp_output_shadow[QSFP_CAGE_COUNT] = {QSFP_OUT_IDLE, QSFP_OUT_IDLE, QSFP_OUT_IDLE,
					       QSFP_OUT_IDLE};

K_MUTEX_DEFINE(qsfp_mutex);
static uint8_t expanders_ready;

void qsfp_hold_translator_off(void)
{
	const struct gpio_dt_spec oe = GPIO_DT_SPEC_GET(DT_NODELABEL(qsfp_i2c_en), gpios);
	int ret = gpio_pin_configure_dt(&oe, GPIO_OUTPUT_INACTIVE);

	if (ret != 0) {
		LOG_ERR("QSFP: failed to hold MCU_CONN_I2C_EN low: %d", ret);
	}
}

void qsfp_enable_translator(void)
{
	const struct gpio_dt_spec oe = GPIO_DT_SPEC_GET(DT_NODELABEL(qsfp_i2c_en), gpios);
	int ret = gpio_pin_configure_dt(&oe, GPIO_OUTPUT_ACTIVE);

	if (ret != 0) {
		LOG_ERR("QSFP: failed to enable MCU_CONN_I2C_EN: %d", ret);
		return;
	}
	k_msleep(2);
}

int qsfp_write_output(const struct device *bus, uint8_t cage, uint8_t value)
{
	int ret;

	if (cage >= ARRAY_SIZE(qsfp_cages)) {
		return -EINVAL;
	}
	ret = i2c_reg_write_byte(bus, qsfp_cages[cage].expander_addr, TCA9554_REG_OUTPUT, value);
	if (ret == 0) {
		qsfp_output_shadow[cage] = value | QSFP_BIT_MODSELL;
	}
	return ret;
}

static int qsfp_park_all(const struct device *bus)
{
	int ret = 0;
	int err;

	ARRAY_FOR_EACH(qsfp_cages, i) {
		if ((expanders_ready & BIT(i)) == 0) {
			continue;
		}
		err = i2c_reg_write_byte(bus, qsfp_cages[i].expander_addr, TCA9554_REG_OUTPUT,
					 qsfp_output_shadow[i] | QSFP_BIT_MODSELL);
		if (err != 0) {
			LOG_ERR("QSFP %s: deselect failed: %d", qsfp_cages[i].name, err);
			if (ret == 0) {
				ret = err;
			}
		}
	}

	return ret;
}

/* Deselect every known cage. If an expander NACKs, it may still hold ModSelL
 * low on the shared 0x50 EEPROM, so isolate the translator rather than
 * selecting another cage.
 */
static int qsfp_repark(const struct device *bus)
{
	int ret = qsfp_park_all(bus);

	if (ret == 0) {
		return 0;
	}

	LOG_ERR("QSFP: aborting access, a cage may still be selected (%d)", ret);
	(void)qsfp_recover(bus);
	if (qsfp_park_all(bus) == 0) {
		return 0;
	}

	qsfp_hold_translator_off();
	expanders_ready = 0;
	return ret;
}

int qsfp_select(const struct device *bus, uint8_t cage)
{
	int ret;

	if (cage >= ARRAY_SIZE(qsfp_cages)) {
		return -EINVAL;
	}

	ret = qsfp_repark(bus);
	if (ret != 0) {
		return ret;
	}
	if ((expanders_ready & BIT(cage)) == 0) {
		return -ENODEV;
	}

	return i2c_reg_write_byte(bus, qsfp_cages[cage].expander_addr, TCA9554_REG_OUTPUT,
				  qsfp_output_shadow[cage] & ~QSFP_BIT_MODSELL);
}

void qsfp_deselect(const struct device *bus, uint8_t cage)
{
	if (cage >= ARRAY_SIZE(qsfp_cages) || (expanders_ready & BIT(cage)) == 0) {
		return;
	}
	if (i2c_reg_write_byte(bus, qsfp_cages[cage].expander_addr, TCA9554_REG_OUTPUT,
			       qsfp_output_shadow[cage] | QSFP_BIT_MODSELL) != 0) {
		(void)qsfp_repark(bus);
	}
}

static int qsfp_init_expanders(const struct device *bus)
{
	if (expanders_ready == GENMASK(ARRAY_SIZE(qsfp_cages) - 1, 0)) {
		return qsfp_park_all(bus);
	}

	ARRAY_FOR_EACH(qsfp_cages, i) {
		if ((expanders_ready & BIT(i)) != 0) {
			continue;
		}
		if (i2c_reg_write_byte(bus, qsfp_cages[i].expander_addr, TCA9554_REG_OUTPUT,
				       qsfp_output_shadow[i]) != 0) {
			continue;
		}
		if (i2c_reg_write_byte(bus, qsfp_cages[i].expander_addr, TCA9554_REG_CONFIG,
				       QSFP_CONFIG_DIR) == 0) {
			expanders_ready |= BIT(i);
		}
	}

	if (expanders_ready == 0) {
		return -ENODEV;
	}
	return qsfp_park_all(bus);
}

const struct device *qsfp_session_begin(void)
{
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c3));

	k_mutex_lock(&qsfp_mutex, K_FOREVER);
	if (!device_is_ready(bus)) {
		k_mutex_unlock(&qsfp_mutex);
		return NULL;
	}
	qsfp_enable_translator();
	if (qsfp_init_expanders(bus) != 0) {
		(void)qsfp_repark(bus);
	}
	return bus;
}

void qsfp_session_end(const struct device *bus)
{
	(void)qsfp_repark(bus);
	k_mutex_unlock(&qsfp_mutex);
}

int qsfp_recover(const struct device *bus)
{
	int ret = i2c_recover_bus(bus);

	if (ret != 0) {
		qsfp_hold_translator_off();
		expanders_ready = 0;
	}
	return ret;
}

void qsfp_emergency_park(void)
{
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c3));

	if (!device_is_ready(bus) || k_mutex_lock(&qsfp_mutex, K_NO_WAIT) != 0) {
		return;
	}
	(void)qsfp_repark(bus);
	k_mutex_unlock(&qsfp_mutex);
}
