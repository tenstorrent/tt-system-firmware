/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <tenstorrent/bh_chip.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>

void bh_chip_set_straps(struct bh_chip *chip)
{
	int ret;

	bharc_enable_i2cbus(&chip->config.arc);
	const struct gpio_dt_spec straps[] = {
		chip->config.strapping.gpio6,
		chip->config.strapping.gpio38,
		chip->config.strapping.gpio39,
		chip->config.strapping.gpio40,
	};

	ARRAY_FOR_EACH_PTR(straps, strap_ptr) {
		if (strap_ptr->port != NULL) {
			ret = gpio_pin_configure_dt(strap_ptr, GPIO_OUTPUT_ACTIVE);
			if (ret < 0) {
				printk("Failed to configure strap %s: %d", strap_ptr->port->name,
				       ret);
				i2c_recover_bus(chip->config.arc.i2c_dev);
				ret = gpio_pin_configure_dt(strap_ptr, GPIO_OUTPUT_ACTIVE);
				if (ret < 0) {
					printk("Failed to configure strap after i2c recover %s: "
					       "%d\n",
					       strap_ptr->port->name, ret);
				} else {
					printk("Strap %s successfully configured after i2c "
					       "recover\n",
					       strap_ptr->port->name);
				}
			}
		}
	}
	bharc_disable_i2cbus(&chip->config.arc);
}

void bh_chip_unset_straps(struct bh_chip *chip)
{
	bharc_enable_i2cbus(&chip->config.arc);
	const struct gpio_dt_spec straps[] = {
		chip->config.strapping.gpio6,
		chip->config.strapping.gpio38,
		chip->config.strapping.gpio39,
		chip->config.strapping.gpio40,
	};

	ARRAY_FOR_EACH_PTR(straps, strap_ptr) {
		if (strap_ptr->port != NULL) {
			gpio_pin_configure_dt(strap_ptr, GPIO_INPUT);
		}
	}
	bharc_disable_i2cbus(&chip->config.arc);
}

#ifdef CONFIG_GPIO_PCA_SERIES_INIT_PRIORITY
BUILD_ASSERT(CONFIG_TT_I2C_STRAP_INIT_PRIORITY < CONFIG_GPIO_PCA_SERIES_INIT_PRIORITY,
	     "I2C straps init must run before GPIO init");
#endif

int i2c_straps(void)
{
	ARRAY_FOR_EACH_BH_CHIP(chip) {
		/* Enable I2C bus connection for strapping */
		bharc_enable_i2cbus(&chip->config.arc);
	}
	return 0;
}

int deinit_i2c_straps(void)
{
	ARRAY_FOR_EACH_BH_CHIP(chip) {
		/* Disable I2C bus connection for strapping */
		bharc_disable_i2cbus(&chip->config.arc);
	}
	return 0;
}

SYS_INIT(i2c_straps, POST_KERNEL, CONFIG_TT_I2C_STRAP_INIT_PRIORITY);
SYS_INIT(deinit_i2c_straps, APPLICATION, CONFIG_TT_I2C_STRAP_INIT_PRIORITY);
