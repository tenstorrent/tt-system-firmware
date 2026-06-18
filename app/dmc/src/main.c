/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdlib.h>

#include <app_version.h>
#include <tenstorrent/bist.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/smbus.h>
#include <zephyr/drivers/jtag.h>
#include <zephyr/drivers/mfd/max6639.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <zephyr/dfu/mcuboot.h>

#include <tenstorrent/bh_chip.h>
#include <tenstorrent/bh_arc.h>
#include <tenstorrent/event.h>
#include <tenstorrent/jtag_bootrom.h>
#include <tenstorrent/log_backend_ringbuf.h>
#include <tenstorrent/tt_smbus_regs.h>

#define RESET_UNIT_ARC_PC_CORE_0 0x80030C00

#define INITIAL_FAN_SPEED 35
#define LED_BLINK_RATE_MS 400

LOG_MODULE_REGISTER(main, CONFIG_TT_APP_LOG_LEVEL);

BUILD_ASSERT(IS_ENABLED(CONFIG_TT_BH_ARC_EMUL) || PARTITION_EXISTS(bmfw),
	     "bmfw fixed-partition does not exist");

struct bh_chip BH_CHIPS[BH_CHIP_COUNT] = {DT_FOREACH_PROP_ELEM(DT_PATH(chips), chips, INIT_CHIP)};

#if BH_CHIP_PRIMARY_INDEX >= BH_CHIP_COUNT
#error "Primary chip out of range"
#endif

static const struct gpio_dt_spec board_fault_led =
	GPIO_DT_SPEC_GET_OR(DT_PATH(board_fault_led), gpios, {0});
static const struct device *const ina228 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(ina228));
static const struct device *const max6639_pwm_dev =
	DEVICE_DT_GET_OR_NULL(DT_NODELABEL(max6639_pwm));
static const struct device *const max6639_sensor_dev =
	DEVICE_DT_GET_OR_NULL(DT_NODELABEL(max6639_sensor));

/* No mechanism for getting bl version... yet */
static dmStaticInfo static_info = {.version = 1, .bl_version = 0, .app_version = APPVERSION};

static uint16_t max_power;

/* FIXME: notify_smcs should be automatic, we should notify if the SMCs are ready, otherwise
 * record a notification to be sent once they are. Also it's properly per-SMC state.
 */
void update_fan_speed(bool notify_smcs)
{
	if (DT_NODE_HAS_STATUS(DT_ALIAS(fan0), okay)) {
		uint8_t fan_speed = 0;
		uint8_t forced_fan_speed = 0;

		ARRAY_FOR_EACH_PTR(BH_CHIPS, chip) {
			fan_speed = MAX(fan_speed, chip->data.fan_speed);
			forced_fan_speed =
				MAX(forced_fan_speed,
				    chip->data.fan_speed_forced ? chip->data.fan_speed : 0);
		}

		if (forced_fan_speed != 0) {
			fan_speed = forced_fan_speed;
		}

		uint32_t fan_speed_pwm = DIV_ROUND_UP(fan_speed * UINT8_MAX, 100);

		pwm_set_cycles(max6639_pwm_dev, 0, UINT8_MAX, fan_speed_pwm, 0);

		if (notify_smcs) {
			/* Broadcast final speed to all SMCs for telemetry */
			ARRAY_FOR_EACH_PTR(BH_CHIPS, chip) {
				bharc_smbus_word_data_write(&chip->config.arc, CMFW_SMBUS_FAN_SPEED,
							    fan_speed);
			}
		}
	}
}

static bool process_reset_req(struct bh_chip *chip, uint8_t msg_id, uint32_t msg_data)
{
	switch (msg_data) {
	case kCm2DmResetLevelAsic:
		LOG_INF("Received ARC reset request");
		bh_chip_cancel_bus_transfer_clear(chip);
		bh_chip_reset_chip(chip, true);
		break;

	case kCm2DmResetLevelDmc:
		/* Trigger reboot; will reset asic and reload dmfw */
		LOG_INF("Received system reset request");
		if (IS_ENABLED(CONFIG_REBOOT)) {
			sys_reboot(SYS_REBOOT_COLD);
		}
		break;
	}

	return true;
}

static bool process_ping(struct bh_chip *chip, uint8_t msg_id, uint32_t msg_data)
{
	/* Respond to ping request from CMFW */
	int32_t ret;
	uint32_t retries = 0;

	do {
		uint16_t data = 0xA5A5;

		if (msg_data == 0) {
			ret = bharc_smbus_word_data_read(&chip->config.arc, CMFW_SMBUS_PING_V2,
							 &data);
		} else {
			ret = bharc_smbus_word_data_write(&chip->config.arc, CMFW_SMBUS_PING, data);
		}
		retries++;
	} while (ret != 0U && retries < 10);

	return false;
}

static bool process_fan_speed_update(struct bh_chip *chip, uint8_t msg_id, uint32_t msg_data)
{
	chip->data.fan_speed = FIELD_GET(GENMASK(7, 0), msg_data);
	chip->data.fan_speed_forced = false;
	update_fan_speed(true);

	return false;
}

static bool process_forced_fan_speed_update(struct bh_chip *chip, uint8_t msg_id, uint32_t msg_data)
{
	chip->data.fan_speed = FIELD_GET(GENMASK(7, 0), msg_data);
	chip->data.fan_speed_forced = true;
	update_fan_speed(true);

	return false;
}

static bool process_id_ready(struct bh_chip *chip, uint8_t msg_id, uint32_t msg_data)
{
	chip->data.arc_needs_init_msg = true;
	return false;
}

static bool process_auto_reset_timeout_update(struct bh_chip *chip, uint8_t msg_id,
					      uint32_t msg_data)
{
	/* Set auto reset timeout */
	chip->data.auto_reset_timeout = msg_data;
	if (chip->data.auto_reset_timeout != 0) {
		/* Start auto-reset timer */
		k_timer_start(&chip->auto_reset_timer, K_MSEC(chip->data.auto_reset_timeout),
			      K_NO_WAIT);
	} else {
		/* Stop auto-reset timer */
		k_timer_stop(&chip->auto_reset_timer);
	}
	return false;
}

static bool process_heartbeat_update(struct bh_chip *chip, uint8_t msg_id, uint32_t msg_data)
{
	/* Update telemetry heartbeat */
	if (chip->data.telemetry_heartbeat != msg_data) {
		/* Telemetry heartbeat is moving */
		chip->data.telemetry_heartbeat = msg_data;
		if (chip->data.auto_reset_timeout != 0) {
			/* Restart auto reset timer */
			k_timer_start(&chip->auto_reset_timer,
				      K_MSEC(chip->data.auto_reset_timeout), K_NO_WAIT);
		}
	}
	return false;
}

static void blink_led_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	gpio_pin_toggle_dt(&board_fault_led);
}
static K_TIMER_DEFINE(blink_led_timer, blink_led_expired, NULL);

static bool process_led_blink_request(struct bh_chip *chip, uint8_t msg_id, uint32_t msg_data)
{
	/* Do not allow LED blinks in manufacturing. */
	if (IS_ENABLED(CONFIG_TT_ASSEMBLY_TEST)) {
		return false;
	}

	if (msg_data) {
		k_timer_start(&blink_led_timer, K_MSEC(LED_BLINK_RATE_MS),
			      K_MSEC(LED_BLINK_RATE_MS));
	} else {
		k_timer_stop(&blink_led_timer);
		gpio_pin_set_dt(&board_fault_led, 0);
	}

	return false;
}

static bool process_gddr_therm_trip(struct bh_chip *chip, uint8_t msg_id, uint32_t msg_data)
{
	switch (msg_data) {
	case kGddrThermTripReasonInstantaneous:
		LOG_ERR("GDDR thermal trip: instantaneous temp >= %dC",
			GDDR_THERM_TRIP_CRITICAL_TEMP);
		break;
	case kGddrThermTripReasonSustained:
		LOG_ERR("GDDR thermal trip: sustained temp >= %dC for %d minute(s)",
			GDDR_THERM_TRIP_TEMP, GDDR_THERM_TRIP_DURATION_MIN);
		break;
	default:
		LOG_ERR("GDDR thermal trip: unknown reason %u", msg_data);
		break;
	}

	return false;
}

void process_cm2dm_message(struct bh_chip *chip)
{
	typedef bool (*msg_processor_t)(struct bh_chip *chip, uint8_t msg_id, uint32_t msg_data);

	static const msg_processor_t msg_processors[] = {
		[kCm2DmMsgIdResetReq] = process_reset_req,
		[kCm2DmMsgIdPing] = process_ping,
		[kCm2DmMsgIdFanSpeedUpdate] = process_fan_speed_update,
		[kCm2DmMsgIdForcedFanSpeedUpdate] = process_forced_fan_speed_update,
		[kCm2DmMsgIdReady] = process_id_ready,
		[kCm2DmMsgIdAutoResetTimeoutUpdate] = process_auto_reset_timeout_update,
		[kCm2DmMsgTelemHeartbeatUpdate] = process_heartbeat_update,
		[kCm2DmMsgIdLedBlink] = process_led_blink_request,
		[kCm2DmMsgIdGddrThermTrip] = process_gddr_therm_trip,
	};

	for (uint32_t i = 0U; i < kCm2DmMsgCount; i++) {
		cm2dmMessageRet msg = bh_chip_get_cm2dm_message(chip);

		if (msg.ret != 0) {
			/* error already logged by bh_chip_get_cm2dm_message */
			break;
		}

		if (msg.msg.msg_id == kCm2DmMsgIdNull) {
			/* no messages pending, note that seq_num is not valid */
			break;
		}

		if (chip->data.last_cm2dm_seq_num_valid &&
		    chip->data.last_cm2dm_seq_num == msg.msg.seq_num) {
			static uint16_t last_warned_seq_num = UINT16_MAX;

			/* repeat sequence number, indicates ack failure, try again */
			if (msg.msg.seq_num != last_warned_seq_num) {
				LOG_WRN("Received duplicate CM2DM message.");
				last_warned_seq_num = msg.msg.seq_num;
			}
			continue;
		}

		chip->data.last_cm2dm_seq_num_valid = true;
		chip->data.last_cm2dm_seq_num = msg.msg.seq_num;

		if (msg.msg.msg_id < ARRAY_SIZE(msg_processors) && msg_processors[msg.msg.msg_id]) {
			if (msg_processors[msg.msg.msg_id](chip, msg.msg.msg_id, msg.msg.data)) {
				break;
			}
		}
	}
}

void ina228_power_update(void)
{
	struct sensor_value sensor_val;

	sensor_sample_fetch_chan(ina228, SENSOR_CHAN_POWER);
	sensor_channel_get(ina228, SENSOR_CHAN_POWER, &sensor_val);

	/* Only use integer part of sensor value */
	int16_t power = sensor_val.val1 & 0xFFFF;

	ARRAY_FOR_EACH_PTR(BH_CHIPS, chip) {
		bh_chip_set_input_power(chip, power);
	}
}

uint16_t detect_max_power(void)
{
	static const struct gpio_dt_spec psu_sense0 =
		GPIO_DT_SPEC_GET_OR(DT_PATH(psu_sense0), gpios, {0});
	static const struct gpio_dt_spec psu_sense1 =
		GPIO_DT_SPEC_GET_OR(DT_PATH(psu_sense1), gpios, {0});

	gpio_pin_configure_dt(&psu_sense0, GPIO_INPUT);
	gpio_pin_configure_dt(&psu_sense1, GPIO_INPUT);

	int sense0_val = gpio_pin_get_dt(&psu_sense0);
	int sense1_val = gpio_pin_get_dt(&psu_sense1);

	uint16_t psu_power;

	if (!sense0_val && !sense1_val) {
		psu_power = 600;
	} else if (sense0_val && !sense1_val) {
		psu_power = 450;
	} else if (!sense0_val && sense1_val) {
		psu_power = 300;
	} else {
		/* Pins could either be open or shorted together */
		/* Pull down one and check the other */
		gpio_pin_configure_dt(&psu_sense0, GPIO_OUTPUT_LOW);
		if (!gpio_pin_get_dt(&psu_sense1)) {
			/* If shorted together then max power is 150W */
			psu_power = 150;
		} else {
			psu_power = 0;
		}
		gpio_pin_configure_dt(&psu_sense0, GPIO_INPUT);
	}

	return psu_power;
}

/*
 * Runs a series of SMBUS tests when `CONFIG_DMC_RUN_SMBUS_TESTS` is enabled.
 * These tests aren't intended to be run on production firmware.
 */
static int bh_chip_run_smbus_tests(struct bh_chip *chip)
{
#ifdef CONFIG_DMC_RUN_SMBUS_TESTS
	int ret;
	int pass_val = 0xFEEDFACE;
	uint8_t count;
	uint8_t data[255]; /* Max size of SMBUS block read */
	uint32_t app_version;

	/* Test SMBUS telemetry by selecting TAG_DM_APP_FW_VERSION and reading it back */
	ret = bharc_smbus_byte_data_write(&chip->config.arc, 0x26, 26);
	if (ret < 0) {
		LOG_DBG("Failed to write to SMBUS telemetry register");
		return ret;
	}
	ret = bharc_smbus_block_read(&chip->config.arc, 0x27, &count, data);
	if (ret < 0) {
		LOG_DBG("Failed to read from SMBUS telemetry register");
		return ret;
	}
	if (count != 7) {
		LOG_DBG("SMBUS telemetry read returned unexpected count: %d", count);
		return -EIO;
	}
	if (data[0] != 0U) {
		LOG_DBG("SMBUS telemetry read returned invalid telem idx");
		return -EIO;
	}
	(void)memcpy(&app_version, &data[3], sizeof(app_version));

	if (app_version != APPVERSION) {
		LOG_DBG("SMBUS telemetry read returned unexpected value: %08x", *(uint32_t *)data);
		return -EIO;
	}

	/* Test block write block read call*/
	uint32_t test_data = 0x1234FEDC;

	ret = bharc_smbus_block_write_block_read(
		&chip->config.arc, CMFW_SMBUS_TEST_WRITE_BLOCK_READ_BLOCK, sizeof(test_data),
		(uint8_t *)&test_data, &count, data);
	if (ret < 0) {
		LOG_DBG("Failed to Perform block write block read command");
		return ret;
	}
	(void)memcpy(&test_data, data, sizeof(test_data));
	if (test_data != 0x1234FEDC) {
		LOG_DBG("Incorrect read back value: Expected 0x1234FEDC; actual:0x%08X", test_data);
		return -EIO;
	}

	/* Record test status into scratch register */
	ret = bharc_smbus_block_write(&chip->config.arc, 0xDD, sizeof(pass_val),
				      (uint8_t *)&pass_val);
	if (ret < 0) {
		LOG_DBG("Failed to write to SMBUS scratch register");
		return ret;
	}
	printk("SMBUS tests passed\n");
#endif
	return 0;
}

static void handle_therm_trip(void)
{
	ARRAY_FOR_EACH_PTR(BH_CHIPS, chip) {
		if (chip->data.therm_trip_triggered) {
			chip->data.therm_trip_triggered = false;

			if (board_fault_led.port != NULL) {
				gpio_pin_set_dt(&board_fault_led, 1);
			}

			/* hold fan at 100% until we hear otherwise from this chip */
			chip->data.fan_speed = 100;
			chip->data.fan_speed_forced = true;

			if (DT_NODE_HAS_STATUS(DT_ALIAS(fan0), okay)) {
				pwm_set_cycles(max6639_pwm_dev, 0, UINT8_MAX, UINT8_MAX, 0);
			}

			/* Prioritize the system rebooting over the therm trip handler */
			if (!atomic_get(&chip->data.trigger_reset)) {
				/* Technically trigger_reset could have been set here but I
				 * think I'm happy to eat the non-enum in that case
				 */
				chip->data.performing_reset = true;
				/* Set the bus cancel following the logic of
				 * (reset_triggered && !performing_reset)
				 */
				bh_chip_cancel_bus_transfer_clear(chip);

				chip->data.therm_trip_count++;
				bh_chip_reset_chip(chip, true);

				/* Set the bus cancel following the logic of
				 * (reset_triggered && !performing_reset)
				 */
				if (atomic_get(&chip->data.trigger_reset)) {
					bh_chip_cancel_bus_transfer_set(chip);
				}
				chip->data.performing_reset = false;
			}
		}
	}
}

static void handle_watchdog_reset(void)
{
	ARRAY_FOR_EACH_PTR(BH_CHIPS, chip) {
		if (chip->data.arc_wdog_triggered) {
			chip->data.arc_wdog_triggered = false;
			bh_chip_cancel_bus_transfer_clear(chip);
			/* Read PC from ARC and record it */
			jtag_setup(chip->config.jtag);
			jtag_reset(chip->config.jtag);
			jtag_axi_read32(chip->config.jtag, RESET_UNIT_ARC_PC_CORE_0,
					&chip->data.arc_hang_pc);
			jtag_teardown(chip->config.jtag);
			/* Clear watchdog state */
			chip->data.auto_reset_timeout = 0;

			/* hold fan at 100% until we hear otherwise from this chip */
			chip->data.fan_speed = 100;
			chip->data.fan_speed_forced = true;

			if (DT_NODE_HAS_STATUS(DT_ALIAS(fan0), okay)) {
				pwm_set_cycles(max6639_pwm_dev, 0, UINT8_MAX, UINT8_MAX, 0);
			}

			chip->data.performing_reset = true;
			bh_chip_reset_chip(chip, true);
			/* Clear bus transfer cancel flag */
			bh_chip_cancel_bus_transfer_clear(chip);

			chip->data.performing_reset = false;
		}
	}
}

static void handle_perst(void)
{
	ARRAY_FOR_EACH_PTR(BH_CHIPS, chip) {
		if (atomic_set(&chip->data.trigger_reset, false)) {
			chip->data.performing_reset = true;
			chip->data.last_cm2dm_seq_num_valid = false;
			/*
			 * Set the bus cancel following the logic of (reset_triggered &&
			 * !performing_reset)
			 */
			bh_chip_cancel_bus_transfer_clear(chip);

			bharc_disable_i2cbus(&chip->config.arc);
			jtag_bootrom_reset_asic(chip);
			jtag_bootrom_set_cable_power_limit(chip, chip->data.cable_power_limit);
			jtag_bootrom_soft_reset_arc(chip);
			jtag_bootrom_teardown(chip);
			bharc_enable_i2cbus(&chip->config.arc);

			/*
			 * Set the bus cancel following the logic of (reset_triggered &&
			 * !performing_reset)
			 */
			if (atomic_get(&chip->data.trigger_reset)) {
				bh_chip_cancel_bus_transfer_set(chip);
			}
			chip->data.therm_trip_count = 0;
			chip->data.arc_hang_pc = 0;
			chip->data.performing_reset = false;
		}
	}
}

static void handle_pgood_change(void)
{
	ARRAY_FOR_EACH_PTR(BH_CHIPS, chip) {
		handle_pgood_event(chip, board_fault_led);
	}
}

static void send_init_data(void)
{
	ARRAY_FOR_EACH_PTR(BH_CHIPS, chip) {
		if (chip->data.arc_needs_init_msg) {
			if (bh_chip_set_static_info(chip, &static_info) == 0 &&
			    bh_chip_set_input_power_lim(chip, max_power) == 0 &&
			    bh_chip_set_therm_trip_count(chip, chip->data.therm_trip_count) == 0 &&
			    bh_chip_run_smbus_tests(chip) == 0) {
				chip->data.arc_needs_init_msg = false;
			}
		}
	}
}

static void board_power_update(void)
{
	if (IS_ENABLED(CONFIG_INA228)) {
		ina228_power_update();
	}
}

static void fan_rpm_feedback(void)
{
	if (DT_NODE_HAS_STATUS(DT_ALIAS(fan0), okay)) {
		uint16_t rpm;
		struct sensor_value data;

		sensor_sample_fetch_chan(max6639_sensor_dev, MAX6639_CHAN_1_RPM);
		sensor_channel_get(max6639_sensor_dev, MAX6639_CHAN_1_RPM, &data);

		rpm = (uint16_t)data.val1;

		ARRAY_FOR_EACH_PTR(BH_CHIPS, chip) {
			bh_chip_set_fan_rpm(chip, rpm);
		}
	}
}

static void handle_cm2dm_messages(void)
{
	ARRAY_FOR_EACH_PTR(BH_CHIPS, chip) {
		process_cm2dm_message(chip);
	}
}

static void send_logs_to_smc(void)
{
	uint8_t *log_data;
	int ret;

	/* Pull up to 32 bytes from the ringbuf log backend */
	ret = log_backend_ringbuf_get_claim(&log_data, 32);
	if (ret > 0) {
		/* Write log data to the first BH chip */
		if (bh_chip_write_logs(&BH_CHIPS[BH_CHIP_PRIMARY_INDEX], log_data, ret) == 0) {
			/* Only finish the claim if the write was successful */
			log_backend_ringbuf_finish_claim(ret);
		} else {
			/* Otherwise, indicate we consumed 0 bytes */
			log_backend_ringbuf_finish_claim(0);
		}
	}
}

static void shared_20ms_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	tt_event_post(TT_EVENT_FAN_RPM_TO_SMC | TT_EVENT_CM2DM_POLL | TT_EVENT_LOGS_TO_SMC);
}
static K_TIMER_DEFINE(shared_20ms_event_timer, shared_20ms_expired, NULL);

static void board_power_update_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	tt_event_post(TT_EVENT_BOARD_POWER_TO_SMC);
}
static K_TIMER_DEFINE(board_power_update_timer, board_power_update_expired, NULL);

int main(void)
{
	int ret;
	int bist_rc;

	bist_rc = 0;
	if (IS_ENABLED(CONFIG_TT_BIST)) {
		bist_rc = tt_bist();
		if (bist_rc < 0) {
			LOG_ERR("%s() failed: %d", "tt_bist", bist_rc);
		} else {
			LOG_DBG("Built-in self-test succeeded");
		}
	}

	ARRAY_FOR_EACH_PTR(BH_CHIPS, chip) {
		chip->data.fan_speed = INITIAL_FAN_SPEED;
	}

	update_fan_speed(false);

	if (bist_rc == 0 && !boot_is_img_confirmed()) {
		ret = boot_write_img_confirmed();
		if (ret < 0) {
			LOG_DBG("%s() failed: %d", "boot_write_img_confirmed", ret);
			return ret;
		}
		LOG_INF("Firmware update is confirmed.");
	}

	/* Force all spi_muxes back to arc control */
	ARRAY_FOR_EACH_PTR(BH_CHIPS, chip) {
		if (chip->config.spi_mux.port != NULL) {
			gpio_pin_configure_dt(&chip->config.spi_mux, GPIO_OUTPUT_ACTIVE);
		}
	}

	/* Set up GPIOs */
	if (board_fault_led.port != NULL) {
		gpio_pin_configure_dt(&board_fault_led, GPIO_OUTPUT_INACTIVE);
	}

	ARRAY_FOR_EACH_PTR(BH_CHIPS, chip) {
		ret = therm_trip_gpio_setup(chip);
		if (ret != 0) {
			LOG_ERR("%s() failed: %d", "therm_trip_gpio_setup", ret);
			return ret;
		}
		ret = pgood_gpio_setup(chip);
		if (ret != 0) {
			LOG_ERR("%s() failed: %d", "pgood_gpio_setup", ret);
			return ret;
		}
	}

	/* Detect cable power limit early, before JTAG bootrom sequence.
	 * This value will be written to a scratch register for SMC to read at boot.
	 */
	max_power = detect_max_power();

	if (IS_ENABLED(CONFIG_JTAG_LOAD_BOOTROM)) {
		ARRAY_FOR_EACH_PTR(BH_CHIPS, chip) {
			ret = jtag_bootrom_init(chip);
			if (ret != 0) {
				LOG_ERR("%s() failed: %d", "jtag_bootrom_init", ret);
				return ret;
			}

			bharc_disable_i2cbus(&chip->config.arc);

			/* Store power limit for use in runtime resets */
			chip->data.cable_power_limit = max_power;
			ret = jtag_bootrom_reset_sequence(chip, false, max_power);
			/* Always enable I2C bus */
			bharc_enable_i2cbus(&chip->config.arc);
			if (ret != 0) {
				LOG_ERR("%s() failed: %d", "jtag_bootrom_reset", ret);
				return ret;
			}
		}

		LOG_DBG("Bootrom workaround successfully applied");
	}

	ARRAY_FOR_EACH_PTR(BH_CHIPS, chip) {
		const struct device *smbus = chip->config.arc.smbus.bus;

		smbus_configure(smbus, SMBUS_MODE_CONTROLLER | SMBUS_MODE_PEC);
		bh_chip_cancel_bus_transfer_clear(chip);
	}

	printk("DMFW VERSION " APP_VERSION_STRING "\n");

	/* For manufacturing, keep the red LED solid on so it can be visually inspected. */
	if (IS_ENABLED(CONFIG_TT_ASSEMBLY_TEST) && board_fault_led.port != NULL) {
		gpio_pin_set_dt(&board_fault_led, 1);
	}

	k_timer_start(&shared_20ms_event_timer, K_MSEC(20), K_MSEC(20));
	k_timer_start(&board_power_update_timer, K_MSEC(1), K_MSEC(1));

	while (true) {
		uint32_t events = tt_event_wait(TT_EVENT_ANY, K_FOREVER);

		/* These are urgent events, and gated by their own flags. */
		handle_therm_trip();

		handle_watchdog_reset();

		handle_perst();

		handle_pgood_change();

		/* send_init_data only triggers once per chip (per reset). */
		send_init_data();

		if (events & (TT_EVENT_BOARD_POWER_TO_SMC | TT_EVENT_WAKE)) {
			board_power_update();
		}

		if (events & (TT_EVENT_FAN_RPM_TO_SMC | TT_EVENT_WAKE)) {
			fan_rpm_feedback();
		}

		if (events & (TT_EVENT_CM2DM_POLL | TT_EVENT_WAKE)) {
			handle_cm2dm_messages();
		}

		if (events & (TT_EVENT_LOGS_TO_SMC | TT_EVENT_WAKE)) {
			send_logs_to_smc();
		}
	}

	return EXIT_SUCCESS;
}
