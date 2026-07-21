/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bh_reg_def.h"
#include "status_reg.h"

#include <stdint.h>

#include <tenstorrent/bh_chip.h>
#include <tenstorrent/event.h>
#include <tenstorrent/jtag_bootrom.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/jtag.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(jtag_bootrom, CONFIG_TT_JTAG_BOOTROM_LOG_LEVEL);

static uint32_t perst_start_time;

bool jtag_axiwait(const struct device *dev, uint32_t addr)
{
	/* If we are using the emulated driver then always return true */
	if (DT_HAS_COMPAT_STATUS_OKAY(zephyr_gpio_emul)) {
		return true;
	}

	jtag_reset(dev);

	/* Returns true on g2g */
	uint32_t value = 0;

	return !jtag_axi_read32(dev, addr, &value);
}

void jtag_bitbang_wait_for_id(const struct device *dev)
{
	uint32_t reset_id = 0;

	while (true) {
		jtag_reset(dev);
		jtag_read_id(dev, &reset_id);

		if (reset_id == 0x138A5) {
			break;
		}
		k_yield();
	}
}

static const __maybe_unused struct gpio_dt_spec arc_rambus_jtag_mux_sel =
	GPIO_DT_SPEC_GET_OR(DT_NODELABEL(arc_rambus_jtag_mux_sel), gpios, {0});
static const __maybe_unused struct gpio_dt_spec arc_l2_jtag_mux_sel =
	GPIO_DT_SPEC_GET_OR(DT_NODELABEL(arc_l2_jtag_mux_sel), gpios, {0});

#ifdef CONFIG_JTAG_LOAD_ON_PRESET
static const struct gpio_dt_spec preset_trigger = GPIO_DT_SPEC_GET(DT_PATH(preset_trigger), gpios);

void gpio_asic_reset_callback(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	perst_start_time = k_cycle_get_32();

	ARRAY_FOR_EACH_BH_CHIP(chip) {
		chip->data.perst_seen = true;
		atomic_set(&chip->data.trigger_reset, true);
		/* Set the bus cancel following the logic of (reset_triggered && !performing_reset)
		 */
		if (!chip->data.performing_reset) {
			bh_chip_cancel_bus_transfer_set(chip);
		}
	}
	tt_event_post(TT_EVENT_PERST);
}

static struct gpio_callback preset_cb_data;
#endif /* IS_ENABLED(CONFIG_JTAG_LOAD_ON_PRESET) */

int jtag_trigger_mem_repair(struct bh_chip *chip)
{
#ifdef CONFIG_JTAG_LOAD_BOOTROM
	const struct device *dev = chip->config.jtag;

	int64_t start_cycles = k_cycle_get_64();
	int64_t deadline_cycles = start_cycles + k_us_to_cyc_floor64(20000);

	LOG_DBG("start mem_repair at %lld us since boot", k_cyc_to_us_floor64(start_cycles));

	/* Start mem repair */
	jtag_axi_write32(dev, RESET_UNIT_MEM_REPAIR_CNTL_REG_ADDR, 1);

	uint32_t mem_repair_status;

	do {
		jtag_axi_read32(dev, RESET_UNIT_MEM_REPAIR_STATUS_REG_ADDR, &mem_repair_status);
		int64_t current_cycles = k_cycle_get_64();

		if (current_cycles - deadline_cycles >= 0) {
			LOG_ERR("mem repair timed out at %lld us",
				k_cyc_to_us_floor64(current_cycles));
			return -ETIMEDOUT;
		}
	} while (mem_repair_status == 0);

	LOG_DBG("mem repair finished at %lld us, status=0x%08x",
		k_cyc_to_us_floor64(k_cycle_get_64()), mem_repair_status);
#else
	ARG_UNUSED(chip);
#endif
	return 0;
}

int jtag_bootrom_reset_asic(struct bh_chip *chip)
{
	/* Only check for pgood if we aren't emulating */
#if !DT_HAS_COMPAT_STATUS_OKAY(zephyr_gpio_emul)
	if (!gpio_pin_get_dt(&chip->config.pgood)) {
		printk("Waiting for pgood to rise...\n");
	}
	while (!gpio_pin_get_dt(&chip->config.pgood)) {
	}
#endif

	bh_chip_assert_asic_reset(chip);
	bh_chip_assert_spi_reset(chip);

	int ret = jtag_setup(chip->config.jtag);

	if (ret) {
		return ret;
	}

	/* k_sleep(K_MSEC(1)); */
	k_busy_wait(1000);

	bh_chip_set_straps(chip);

	bh_chip_deassert_asic_reset(chip);
	bh_chip_deassert_spi_reset(chip);

	/* k_sleep(K_MSEC(2)); */
	k_busy_wait(2000);

	jtag_reset(chip->config.jtag);

#if !DT_HAS_COMPAT_STATUS_OKAY(zephyr_gpio_emul)
	jtag_bitbang_wait_for_id(chip->config.jtag);
#endif

	jtag_reset(chip->config.jtag);

	while (!jtag_axiwait(chip->config.jtag, STATUS_POST_CODE_REG_ADDR)) {
		k_yield();
	}

	jtag_trigger_mem_repair(chip);

	jtag_reset(chip->config.jtag);

	bh_chip_unset_straps(chip);

	return 0;
}

int jtag_bootrom_init(struct bh_chip *chip)
{
	int ret = false;

	if (DT_NODE_EXISTS(DT_NODELABEL(arc_rambus_jtag_mux_sel))) {
		ret |= gpio_pin_configure_dt(&arc_rambus_jtag_mux_sel, GPIO_OUTPUT_ACTIVE);
	}

	if (DT_NODE_EXISTS(DT_NODELABEL(arc_l2_jtag_mux_sel))) {
		ret |= gpio_pin_configure_dt(&arc_l2_jtag_mux_sel, GPIO_OUTPUT_ACTIVE);
	}

	ret |= gpio_pin_configure_dt(&chip->config.pgood, GPIO_INPUT);
	if (ret) {
		return ret;
	}
	ret |= gpio_pin_configure_dt(&chip->config.asic_reset, GPIO_OUTPUT_ACTIVE) ||
	       gpio_pin_configure_dt(&chip->config.spi_reset, GPIO_OUTPUT_ACTIVE);
	if (ret) {
		return ret;
	}

#ifdef CONFIG_JTAG_LOAD_ON_PRESET
	if (chip == &BH_CHIPS[BH_CHIP_PRIMARY_INDEX]) {
		ret = gpio_pin_configure_dt(&preset_trigger, GPIO_INPUT);
		if (ret) {
			return ret;
		}

		ret = gpio_pin_interrupt_configure_dt(&preset_trigger, GPIO_INT_EDGE_TO_INACTIVE);
		if (ret) {
			return ret;
		}

		gpio_init_callback(&preset_cb_data, gpio_asic_reset_callback,
				   BIT(preset_trigger.pin));
		gpio_add_callback(preset_trigger.port, &preset_cb_data);
	}

	/* Active LOW, so will be false if high */
	if (!gpio_pin_get_dt(&preset_trigger)) {
		/* If the preset trigger started high, then we came out of reset with the
		 * system
		 */
		/* thinking that pcie is ready to go. We need to forcibly apply the
		 * workaround to
		 * ensure this remains true.
		 */
		chip->data.trigger_reset = true;
	}
#endif /* IS_ENABLED(CONFIG_JTAG_LOAD_ON_PRESET) */

	return 0;
}

int jtag_bootrom_patch_offset(struct bh_chip *chip, const uint32_t *patch, size_t patch_len,
			      const uint32_t start_addr)
{
#ifdef CONFIG_JTAG_LOAD_BOOTROM
	const struct device *dev = chip->config.jtag;

	jtag_reset(dev);

	/* HALT THE ARC CORE!!!!! */
	uint32_t arc_misc_cntl = 0;

	jtag_axi_read32(dev, RESET_UNIT_ARC_MISC_CNTL_REG_ADDR, &arc_misc_cntl);

	arc_misc_cntl |= (0b1111 << 4);
	jtag_axi_write32(dev, RESET_UNIT_ARC_MISC_CNTL_REG_ADDR, arc_misc_cntl);
	/* Reset it back to zero */
	jtag_axi_read32(dev, RESET_UNIT_ARC_MISC_CNTL_REG_ADDR, &arc_misc_cntl);
	arc_misc_cntl &= ~(0b1111 << 4);
	jtag_axi_write32(dev, RESET_UNIT_ARC_MISC_CNTL_REG_ADDR, arc_misc_cntl);

	/* Enable gpio trien */
	jtag_axi_write32(dev, RESET_UNIT_GPIO_PAD_TRIEN_CNTL_REG_ADDR, 0xff00);

	/* Write to postcode */
	jtag_axi_write32(dev, STATUS_POST_CODE_REG_ADDR, 0xF2);

	jtag_axi_block_write(dev, start_addr, patch, patch_len);

	jtag_axi_write32(dev, STATUS_POST_CODE_REG_ADDR, 0xF3);

	chip->data.workaround_applied = true;
#endif

	return 0;
}

int jtag_bootrom_verify(const struct device *dev, const uint32_t *patch, size_t patch_len)
{
	if (!IS_ENABLED(CONFIG_JTAG_VERIFY_WRITE)) {
		return 0;
	}

	/* Confirmed matching */
	for (int i = 0; i < patch_len; ++i) {
		/* ICCM start addr is 0 */
		uint32_t readback = 0;
#ifdef CONFIG_JTAG_EMUL
		jtag_emul_axi_read32(dev, i * 4, &readback);
#else
		jtag_axi_read32(dev, i * 4, &readback);
#endif

		if (patch[i] != readback) {
			printk("Bootcode mismatch at %03x. expected: %08x actual: %08x "
			       "¯\\_(ツ)_/¯\n",
			       i * 4, patch[i], readback);

			jtag_axi_write32(dev, STATUS_POST_CODE_REG_ADDR, 0x6);
			return 1;
		}
	}

	printk("Bootcode write verified! \\o/\n");

	return 0;
}

uint32_t jtag_bootrom_get_perst_start_time(void)
{
	return perst_start_time;
}

void jtag_bootrom_soft_reset_arc(struct bh_chip *chip)
{
#ifdef CONFIG_JTAG_LOAD_BOOTROM
	const struct device *dev = chip->config.jtag;

	jtag_reset(dev);

	/* HALT THE ARC CORE!!!!! */

	/* NOTE(drosen): Assuming that it is okay to set the register to 0b1111 << 4, this saves
	 * some cycles but may lead to errors in the future.
	 */
	jtag_axi_write32(dev, RESET_UNIT_ARC_MISC_CNTL_REG_ADDR, GENMASK(7, 4));
	/* Reset it back to zero */
	/* NOTE(drosen): Assuming that it is okay to set the register back to zero, this saves some
	 * cycles but may lead to errors in the future.
	 */
	jtag_axi_write32(dev, RESET_UNIT_ARC_MISC_CNTL_REG_ADDR, 0);

	/* Write reset_vector (rom_memory[0]) */
	jtag_axi_write32(dev, ROM_MEMORY_MEM_BASE_ADDR, 0x84);

	/* store DMC init done timestamp */
	if (chip->data.perst_seen) {
		chip->data.dm_init_done = k_cycle_get_32();
		chip->data.perst_seen = false;
	}

	/* store ASIC refclk timestamp of DMC starts bootcode execution as a reference for cmfw. */
	jtag_axi_read32(dev, RESET_UNIT_REFCLK_CNT_LO_REG_ADDR, &chip->data.arc_start_time);

	/* Toggle soft-reset */
	/* ARC_MISC_CNTL.soft_reset (12th bit) */
	/* NOTE(drosen): Assuming that it is okay to set the register to 1 << 12, this saves some
	 * cycles but may lead to errors in the future.
	 */
	jtag_axi_write32(dev, RESET_UNIT_ARC_MISC_CNTL_REG_ADDR, BIT(12));

	/* Set to 0 */
	/* NOTE(drosen): Assuming that it is okay to set the register back to zero, this saves some
	 * cycles but may lead to errors in the future.
	 */
	jtag_axi_write32(dev, RESET_UNIT_ARC_MISC_CNTL_REG_ADDR, 0);
#endif
}

void jtag_bootrom_set_cable_power_limit(struct bh_chip *chip, uint16_t power_limit)
{
#ifdef CONFIG_JTAG_LOAD_BOOTROM
	const struct device *dev = chip->config.jtag;

	/* Write cable power limit with magic marker for SMC to detect feature support.
	 * Format: [31:16] = CABLE_POWER_LIMIT_MAGIC, [15:0] = power_limit
	 * Legacy SMC will read this as a large positive value (not 0), so safe.
	 * New SMC checks for magic marker to enable cable fault detection.
	 * A power_limit of 0 indicates cable fault (no cable or improper installation).
	 */
	uint32_t value = CABLE_POWER_LIMIT_MAGIC | (uint32_t)power_limit;

	jtag_axi_write32(dev, DMC_CABLE_POWER_LIMIT_REG_ADDR, value);
#endif
}

void jtag_bootrom_teardown(const struct bh_chip *chip)
{
	/* Just one more for good luck */
	jtag_reset(chip->config.jtag);
	jtag_teardown(chip->config.jtag);
}
