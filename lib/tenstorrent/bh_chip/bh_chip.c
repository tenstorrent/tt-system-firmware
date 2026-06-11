/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <tenstorrent/jtag_bootrom.h>
#include <tenstorrent/bh_chip.h>
#include <tenstorrent/event.h>
#include <tenstorrent/tt_smbus_regs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/clock.h>
#include <zephyr/drivers/i2c.h>
#include <string.h>

LOG_MODULE_REGISTER(bh_chip, CONFIG_TT_BH_CHIP_LOG_LEVEL);

void bh_chip_cancel_bus_transfer_set(struct bh_chip *chip)
{
	smbus_cancel(chip->config.arc.smbus.bus);
}

void bh_chip_cancel_bus_transfer_clear(struct bh_chip *chip)
{
	smbus_uncancel(chip->config.arc.smbus.bus);
}

cm2dmMessageRet bh_chip_get_cm2dm_message(struct bh_chip *chip)
{
	cm2dmMessageRet output = {
		.ret = -1,
		.ack_ret = -1,
	};
	uint8_t count = sizeof(output.msg);
	uint8_t buf[255]; /* Max SMBus block read */

	output.ret = bharc_smbus_block_read(&chip->config.arc, CMFW_SMBUS_REQ, &count, buf);
	memcpy(&output.msg, buf, sizeof(output.msg));

	if (output.ret == 0 && output.msg.msg_id != 0) {
		cm2dmAck ack = {0};

		ack.msg_id = output.msg.msg_id;
		ack.seq_num = output.msg.seq_num;
		union cm2dmAckWire wire_ack;

		wire_ack.f = ack;
		output.ack = ack;
		output.ack_ret = bharc_smbus_word_data_write(&chip->config.arc, CMFW_SMBUS_ACK,
							     wire_ack.val);
	}

	if (output.ret != 0 || (output.msg.msg_id != kCm2DmMsgIdNull && output.ack_ret != 0)) {
		static k_timepoint_t message_ratelimit;
		static k_timepoint_t recover_ratelimit;

		if (sys_timepoint_expired(message_ratelimit)) {
			message_ratelimit = sys_timepoint_calc(K_SECONDS(1));

			LOG_WRN("CM2DM SMBus communication failed. req: %d ack: %d", output.ret,
				output.ack_ret);
		}

		if (output.ret == -EIO && sys_timepoint_expired(recover_ratelimit)) {
			recover_ratelimit = sys_timepoint_calc(K_MSEC(250));

			i2c_recover_bus(chip->config.arc.i2c_dev);
			smbus_uncancel(chip->config.arc.smbus.bus);
		}
	}

	return output;
}

int bh_chip_set_static_info(struct bh_chip *chip, dmStaticInfo *info)
{
	int ret;

	info->arc_start_time = chip->data.arc_start_time;
	info->dm_init_duration = chip->data.dm_init_done - jtag_bootrom_get_perst_start_time();
	info->arc_hang_pc = chip->data.arc_hang_pc;
	ret = bharc_smbus_block_write(&chip->config.arc, CMFW_SMBUS_DM_STATIC_INFO,
				      sizeof(dmStaticInfo), (uint8_t *)info);

	return ret;
}

int bh_chip_set_input_power(struct bh_chip *chip, uint16_t power)
{
	int ret;

	ret = bharc_smbus_word_data_write(&chip->config.arc, CMFW_SMBUS_POWER_INSTANT, power);

	return ret;
}

int bh_chip_set_input_power_lim(struct bh_chip *chip, uint16_t max_power)
{
	int ret;

	ret = bharc_smbus_word_data_write(&chip->config.arc, CMFW_SMBUS_POWER_LIMIT, max_power);

	return ret;
}

int bh_chip_set_fan_rpm(struct bh_chip *chip, uint16_t rpm)
{
	int ret;

	ret = bharc_smbus_word_data_write(&chip->config.arc, CMFW_SMBUS_FAN_RPM, rpm);

	return ret;
}

int bh_chip_set_therm_trip_count(struct bh_chip *chip, uint16_t therm_trip_count)
{
	int ret;

	ret = bharc_smbus_word_data_write(&chip->config.arc, CMFW_SMBUS_THERM_TRIP_COUNT,
					  therm_trip_count);

	return ret;
}

void bh_chip_auto_reset(struct k_timer *timer)
{
	struct bh_chip *chip = CONTAINER_OF(timer, struct bh_chip, auto_reset_timer);

	chip->data.arc_wdog_triggered = true;
	/* Cancel bus transfers, ARC is likely hung */
	bh_chip_cancel_bus_transfer_set(chip);
	tt_event_post(TT_EVENT_WATCHDOG_EXPIRED);
}

int bh_chip_write_logs(struct bh_chip *chip, char *log_data, size_t log_size)
{
	int ret;

	if (log_size > 32) {
		return -ENOBUFS;
	}

	ret = bharc_smbus_block_write(&chip->config.arc, CMFW_SMBUS_DMC_LOG, log_size,
				      (uint8_t *)log_data);

	return ret;
}

void bh_chip_assert_asic_reset(const struct bh_chip *chip)
{
	gpio_pin_set_dt(&chip->config.asic_reset, 1);
}

void bh_chip_deassert_asic_reset(const struct bh_chip *chip)
{
	gpio_pin_set_dt(&chip->config.asic_reset, 0);
}

void bh_chip_assert_spi_reset(const struct bh_chip *chip)
{
	gpio_pin_set_dt(&chip->config.spi_reset, 1);
}

void bh_chip_deassert_spi_reset(const struct bh_chip *chip)
{
	gpio_pin_set_dt(&chip->config.spi_reset, 0);
}

int bh_chip_reset_chip(struct bh_chip *chip, bool force_reset)
{
	int ret, ret2;

	chip->data.last_cm2dm_seq_num_valid = false;
	ret = bharc_disable_i2cbus(&chip->config.arc);
	if (ret != 0) {
		bharc_enable_i2cbus(&chip->config.arc);
		return ret;
	}

	ret2 = jtag_bootrom_reset_sequence(chip, force_reset, chip->data.cable_power_limit);

	ret = bharc_enable_i2cbus(&chip->config.arc);
	if (ret != 0) {
		return ret;
	}
	return ret2;
}

void therm_trip_detected(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	struct bh_chip *chip = CONTAINER_OF(cb, struct bh_chip, therm_trip_cb);

	chip->data.therm_trip_triggered = true;
	bh_chip_cancel_bus_transfer_set(chip);
	tt_event_post(TT_EVENT_THERM_TRIP);
}

int therm_trip_gpio_setup(struct bh_chip *chip)
{
	/* Set up therm trip interrupt */
	int ret;

	ret = gpio_pin_configure_dt(&chip->config.therm_trip, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("%s() failed: %d", "gpio_pin_configure_dt", ret);
		return ret;
	}
	gpio_init_callback(&chip->therm_trip_cb, therm_trip_detected,
			   BIT(chip->config.therm_trip.pin));
	ret = gpio_add_callback_dt(&chip->config.therm_trip, &chip->therm_trip_cb);
	if (ret != 0) {
		LOG_ERR("%s() failed: %d", "gpio_add_callback_dt", ret);
		return ret;
	}
	ret = gpio_pin_interrupt_configure_dt(&chip->config.therm_trip, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		LOG_ERR("%s() failed: %d", "gpio_pin_interrupt_configure_dt", ret);
	}

	return ret;
}

void pgood_change_detected(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	struct bh_chip *chip = CONTAINER_OF(cb, struct bh_chip, pgood_cb);

	/* Sample PGOOD to see if it rose or fell */
	/* TODO: could setup rising interrupt only after falling triggered */
	if (gpio_pin_get_dt(&chip->config.pgood)) {
		chip->data.pgood_rise_triggered = true;
	} else {
		chip->data.pgood_fall_triggered = true;
	}
	tt_event_post(TT_EVENT_PGOOD);
}

int pgood_gpio_setup(struct bh_chip *chip)
{
	/* Set up PGOOD interrupt */
	int ret;

	ret = gpio_pin_configure_dt(&chip->config.pgood, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("%s() failed: %d", "gpio_pin_configure_dt", ret);
		return ret;
	}
	gpio_init_callback(&chip->pgood_cb, pgood_change_detected, BIT(chip->config.pgood.pin));
	ret = gpio_add_callback_dt(&chip->config.pgood, &chip->pgood_cb);
	if (ret != 0) {
		LOG_ERR("%s() failed: %d", "gpio_add_callback_dt", ret);
		return ret;
	}
	ret = gpio_pin_interrupt_configure_dt(&chip->config.pgood, GPIO_INT_EDGE_BOTH);
	if (ret != 0) {
		LOG_ERR("%s() failed: %d", "gpio_pin_interrupt_configure_dt", ret);
	}

	return ret;
}

void handle_pgood_event(struct bh_chip *chip, struct gpio_dt_spec board_fault_led)
{
	if (chip->data.pgood_fall_triggered && !chip->data.pgood_severe_fault) {
		int64_t current_uptime_ms = k_uptime_get();
		/* Assert board fault */
		gpio_pin_set_dt(&board_fault_led, 1);
		/* Report over SMBus - to add later */
		/* Assert ASIC reset */
		bh_chip_assert_asic_reset(chip);
		/* If pgood went down again within 1 second */
		if (chip->data.pgood_last_trip_ms != 0 &&
		    current_uptime_ms - chip->data.pgood_last_trip_ms < 1000) {
			/* Assert more severe fault over IPMI - to add later */
			chip->data.pgood_severe_fault = true;
		}
		chip->data.pgood_last_trip_ms = current_uptime_ms;
		chip->data.pgood_fall_triggered = false;
	}
	if (chip->data.pgood_rise_triggered && !chip->data.pgood_severe_fault) {
		/* Follow out of reset procedure */
		bh_chip_reset_chip(chip, true);
		/*
		 * Ensure the board fault led will not be deasserted post powercycle during
		 * assembly test
		 */
		if (!IS_ENABLED(CONFIG_TT_ASSEMBLY_TEST)) {
			/* Clear board fault */
			gpio_pin_set_dt(&board_fault_led, 0);
		}
		chip->data.pgood_rise_triggered = false;
	}
}
