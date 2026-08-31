/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_grendel_gpio

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/sys/sys_io.h>

#include <platform.h>

struct gpio_grendel_config {
	/* gpio_driver_config needs to be first */
	struct gpio_driver_config common;
	uint32_t base_addr;
};

struct gpio_grendel_data {
	/* gpio_driver_data needs to be first */
	struct gpio_driver_data common;
};

#define GPIO_REG_SPACING 0x10

static int gpio_grendel_pin_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	const struct gpio_grendel_config *config = dev->config;
	uint32_t reg_addr = config->base_addr + (pin * GPIO_REG_SPACING);
	GPIO_CTRL_CONTROL_reg_u reg;

	reg.val = sys_read32(reg_addr);

	/* Force GPIO mode */
	reg.f.interface_enable = 1;
	/* Use pull settings from gpio register */
	reg.f.config_enable = 1;

	if ((flags & GPIO_OUTPUT) && (flags & GPIO_INPUT)) {
		/* Not supported */
		return -ENOTSUP;
	} else if (flags & GPIO_OUTPUT) {
		reg.f.enable_rx_tx = 0x1; /* Enable TX only */
	} else if (flags & GPIO_INPUT) {
		reg.f.enable_rx_tx = 0x2; /* Enable RX only */
	} else {
		reg.f.enable_rx_tx = 0x0; /* Disable both */
	}

	if (flags & GPIO_OUTPUT_INIT_HIGH) {
		reg.f.chip2pad = 1;
	} else if (flags & GPIO_OUTPUT_INIT_LOW) {
		reg.f.chip2pad = 0;
	}

	if (flags & GPIO_SINGLE_ENDED) {
		/* Setup open drain */
		reg.f.pull_enable_n0_scan = 0; /* Disable pull */
	} else {
		/* Setup pull-up/pull-down */
		reg.f.pull_enable_n0_scan = 1; /* Enable pull */
		if (flags & GPIO_PULL_UP) {
			reg.f.pull_select = 1; /* Pull-up */
		} else if (flags & GPIO_PULL_DOWN) {
			reg.f.pull_select = 0; /* Pull-down */
		}
	}

	sys_write32(reg.val, reg_addr);
	return 0;
}

static int gpio_grendel_port_get_raw(const struct device *dev, uint32_t *value)
{
	const struct gpio_grendel_config *config = dev->config;
	uint8_t pin = 0;
	uint32_t pin_mask = config->common.port_pin_mask;
	GPIO_CTRL_CONTROL_reg_u reg;

	/* Read PAD2SOC for all pins on this instance */
	*value = 0;
	while (pin_mask) {
		reg.val = sys_read32(config->base_addr + (pin * GPIO_REG_SPACING));
		if (reg.f.pad2soc) {
			*value |= BIT(pin);
		}
		pin++;
		pin_mask >>= 1;
	}
	return 0;
}

static int gpio_grendel_port_set_masked_raw(const struct device *dev, uint32_t mask, uint32_t value)
{
	const struct gpio_grendel_config *config = dev->config;
	uint8_t pin = 0;
	uint32_t pin_mask = config->common.port_pin_mask;
	GPIO_CTRL_CONTROL_reg_u reg;

	/* Set CHIP2PAD for all pins where mask is set */
	while (pin_mask) {
		if (mask & BIT(pin)) {
			reg.val = sys_read32(config->base_addr + (pin * GPIO_REG_SPACING));
			reg.f.chip2pad = (value & BIT(pin)) ? 1 : 0;
			sys_write32(reg.val, config->base_addr + (pin * GPIO_REG_SPACING));
		}
		pin++;
		pin_mask >>= 1;
	}
	return 0;
}

static int gpio_grendel_port_set_bits_raw(const struct device *dev, uint32_t pins)
{
	const struct gpio_grendel_config *config = dev->config;
	uint8_t pin = 0;
	uint32_t pin_mask = config->common.port_pin_mask;
	GPIO_CTRL_CONTROL_reg_u reg;

	/* Set CHIP2PAD for all pins where mask is set */
	while (pin_mask) {
		if (pins & BIT(pin)) {
			reg.val = sys_read32(config->base_addr + (pin * GPIO_REG_SPACING));
			reg.f.chip2pad = 1;
			sys_write32(reg.val, config->base_addr + (pin * GPIO_REG_SPACING));
		}
		pin++;
		pin_mask >>= 1;
	}
	return 0;
}

static int gpio_grendel_port_clear_bits_raw(const struct device *dev, uint32_t pins)
{
	const struct gpio_grendel_config *config = dev->config;
	uint8_t pin = 0;
	uint32_t pin_mask = config->common.port_pin_mask;
	GPIO_CTRL_CONTROL_reg_u reg;

	/* Clear CHIP2PAD for all pins where mask is set */
	while (pin_mask) {
		if (pins & BIT(pin)) {
			reg.val = sys_read32(config->base_addr + (pin * GPIO_REG_SPACING));
			reg.f.chip2pad = 0;
			sys_write32(reg.val, config->base_addr + (pin * GPIO_REG_SPACING));
		}
		pin++;
		pin_mask >>= 1;
	}
	return 0;
}

static int gpio_grendel_port_toggle_bits(const struct device *dev, uint32_t pins)
{
	const struct gpio_grendel_config *config = dev->config;
	uint8_t pin = 0;
	uint32_t pin_mask = config->common.port_pin_mask;
	GPIO_CTRL_CONTROL_reg_u reg;

	/* Toggle CHIP2PAD for all pins where mask is set */
	while (pin_mask) {
		if (pins & BIT(pin)) {
			reg.val = sys_read32(config->base_addr + (pin * GPIO_REG_SPACING));
			reg.f.chip2pad = !reg.f.chip2pad;
			sys_write32(reg.val, config->base_addr + (pin * GPIO_REG_SPACING));
		}
		pin++;
		pin_mask >>= 1;
	}
	return 0;
}

static int gpio_grendel_pin_interrupt_configure(const struct device *dev, gpio_pin_t pin,
						enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	/* Interrupts are not supported */
	return -ENOTSUP;
}

static DEVICE_API(gpio, gpio_grendel_api_funcs) = {
	.pin_configure = gpio_grendel_pin_configure,
	.port_get_raw = gpio_grendel_port_get_raw,
	.port_set_masked_raw = gpio_grendel_port_set_masked_raw,
	.port_set_bits_raw = gpio_grendel_port_set_bits_raw,
	.port_clear_bits_raw = gpio_grendel_port_clear_bits_raw,
	.port_toggle_bits = gpio_grendel_port_toggle_bits,
	.pin_interrupt_configure = gpio_grendel_pin_interrupt_configure,
};

static int gpio_grendel_initialize(const struct device *dev)
{
	/* No init required */
	return 0;
}

#define gpio_grendel_INIT(inst)                                                                    \
	static struct gpio_grendel_data gpio_grendel_data_##inst;                                  \
	static const struct gpio_grendel_config gpio_grendel_config_##inst = {                     \
		.common =                                                                          \
			{                                                                          \
				.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_DT_INST(inst),            \
			},                                                                         \
		.base_addr = DT_INST_REG_ADDR(inst),                                               \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, gpio_grendel_initialize, NULL, &gpio_grendel_data_##inst,      \
			      &gpio_grendel_config_##inst, PRE_KERNEL_1,                           \
			      CONFIG_GPIO_INIT_PRIORITY, &gpio_grendel_api_funcs);

DT_INST_FOREACH_STATUS_OKAY(gpio_grendel_INIT)
