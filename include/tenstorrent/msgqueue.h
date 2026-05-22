/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TENSTORRENT_MSGQUEUE_H_
#define TENSTORRENT_MSGQUEUE_H_

#include <stdint.h>

#include <zephyr/sys/iterable_sections.h>

#define NUM_MSG_QUEUES         4
#define MSG_QUEUE_SIZE         4
#define MSG_QUEUE_POINTER_WRAP (2 * MSG_QUEUE_SIZE)
#define REQUEST_MSG_LEN        8
#define RESPONSE_MSG_LEN       8

#define MSG_TYPE_INDEX 0
#define MSG_TYPE_MASK  0xFF
#define MSG_TYPE_SHIFT 0

#define MESSAGE_QUEUE_STATUS_MESSAGE_RECOGNIZED 0xff
#define MESSAGE_QUEUE_STATUS_SCRATCH_ONLY       0xfe

#ifdef __cplusplus
extern "C" {
#endif

struct message_queue_header {
	/* 16B for CPU writes, ARC reads */
	uint32_t request_queue_wptr;
	uint32_t response_queue_rptr;
	uint32_t unused_1;
	uint32_t unused_2;

	/* 16B for ARC writes, CPU reads */
	uint32_t request_queue_rptr;
	uint32_t response_queue_wptr;
	uint32_t last_serial;
	uint32_t unused_3;
};

/**
 * @defgroup tt_msg_apis Host Message Interface
 * @brief Interface for handling host request and response messages between the Tenstorrent host and
 * ARC processor.
 *
 * The host will send a @ref request, specifying the @ref request::command_code (of
 * type @ref tt_smc_msg) SMC firmware will parse this message and send back a @ref response.
 *
 * Specific types of requests are parsed via the union members of @ref request and documented
 * therein.
 * @{
 */

/**
 * @defgroup tt_msg_structs Message Request Structures
 * @ingroup tt_msg_apis
 * @brief Structures and enums used for different types of host requests
 * @{
 */

/** @brief Counter commands */
enum counter_cmd {
	/** @brief Read counter values and status */
	COUNTER_CMD_GET,
	/** @brief Clear selected counters and their overflow bits */
	COUNTER_CMD_CLEAR,
	/** @brief Set the frozen bitmask; frozen counters stop counting */
	COUNTER_CMD_FREEZE,
};

/** @brief Available counter banks */
enum counter_bank {
	/** @brief Throttler counters indexed by @ref aiclk_arb_max */
	COUNTER_BANK_THROTTLERS,
};

/** @brief Host request for generic counter operations
 * @details Messages of this type are dispatched from @c counter.c (bank-specific handlers).
 *
 * For @ref COUNTER_CMD_GET, bank_index selects one counter index
 * within counter_bank (e.g. throttler arbiters 0..N-1).
 * Response:
 * - data[1] bits [31:16]: 1 if that counter is frozen, else 0
 * - data[1] bits [15:0]: 1 if that counter has overflowed since last clear, else 0
 * - data[2]: selected counter value
 *
 * For @ref COUNTER_CMD_CLEAR, each set bit in mask clears that
 * counter to zero and clears its overflow flag.
 *
 * For @ref COUNTER_CMD_FREEZE, mask is the frozen bitmask for
 * the bank (set = frozen, clear = running). Use mask 0 to unfreeze all.
 */
struct counter_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_COUNTER */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief The command to execute, of type @ref counter_cmd */
	uint8_t command;

	/** @brief The counter bank to operate on, of type @ref counter_bank */
	uint8_t counter_bank;

	/** @brief For GET: counter index within @ref counter_bank; ignored for CLEAR/FREEZE */
	uint8_t bank_index;

	/** @brief Reserved; host must set to zero */
	uint8_t reserved[1];

	/** @brief CLEAR (bits to reset) and FREEZE (frozen bitmask) */
	uint16_t mask;
};

/** @brief Host request to force the fan speed */
struct force_fan_speed_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_FORCE_FAN_SPEED*/
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief The raw speed of the fan to set, as a percentage from 0 to 100 */
	uint32_t raw_speed;
};

/** @brief Host request to adjust the AICLK speed
 * @details Requests of this type are processed by @ref aiclk_busy_handler
 */
struct aiclk_set_speed_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_AICLK_GO_BUSY or @ref
	 * TT_SMC_MSG_AICLK_GO_LONG_IDLE
	 */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];
};

/** @brief Host request to start or stop AICLK frequency sweep
 * @details Start messages are processed by @ref SweepAiclkHandler.
 *          Stop messages are processed by @ref SweepAiclkHandler.
 *
 * For @ref TT_SMC_MSG_AISWEEP_START, sweep_low and sweep_high specify the
 * frequency range in MHz. Both must be non-zero.
 * For @ref TT_SMC_MSG_AISWEEP_STOP, no additional arguments are used.
 */
struct aisweep_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_AISWEEP_START or
	 * @ref TT_SMC_MSG_AISWEEP_STOP
	 */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief Low end of sweep range in MHz */
	uint32_t sweep_low;

	/** @brief High end of sweep range in MHz */
	uint32_t sweep_high;
};

/** @brief Host request to force AICLK to a specific frequency
 * @details Messages of this type are processed by @ref ForceAiclkHandler.
 *
 * Set forced_freq to 0 to disable forcing and restore the boot frequency.
 */
struct force_aiclk_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_FORCE_AICLK */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief Frequency to force in MHz, or 0 to disable */
	uint32_t forced_freq;
};

/** @brief Host request to get the current AICLK frequency and control mode
 * @details Messages of this type are processed by @ref get_aiclk_handler.
 *
 * This is a command-only request with no additional arguments.
 * On success, response data[1] contains the current AICLK frequency in MHz
 * and data[2] contains the clock control mode (1 = uncontrolled,
 * 2 = PPM forced, 3 = PPM unforced).
 */
struct get_aiclk_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_GET_AICLK */
	uint8_t command_code;
};

/** @brief Host request to adjust the power settings
 * @details Requests of this type are processed by @ref power_setting_msg_handler
 */
struct power_setting_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_POWER_SETTING*/
	uint8_t command_code;

	/** @brief The number of bits in the @ref power_flags_bitfield that are valid */
	uint8_t power_flags_valid: 4;

	/** @brief The number of fields that are valid in the @ref power_settings_array */
	uint8_t power_settings_valid: 4;

	/** @brief The list of On/Off style power flags SMC supports toggling */
	struct {
		/** @brief 1 - @ref aiclk_update_busy "Set AICLK to Busy" <br>
		 *  0 - @ref aiclk_update_busy "Set AICLK to Idle"
		 */
		uint16_t max_ai_clk: 1;

		/** @brief 1 - @ref set_mrisc_power_setting "Set MRISC power setting to Phy wakeup"
		 * <br> 0 - @ref set_mrisc_power_setting "Set MRISC power setting to Phy Powerdown"
		 */
		uint16_t mrisc_phy_power: 1;

		/** @brief 1 - @ref set_tensix_enable "Enable Tensix cores"
		 * <br> 0 - @ref set_tensix_enable "Disable Tensix cores"
		 */
		uint16_t tensix_enable: 1;

		/** @brief 1 - @ref bh_set_l2cpu_enable "Enable L2CPU cores"
		 * <br> 0 - @ref bh_set_l2cpu_enable "Disable L2CPU cores"
		 */
		uint16_t l2cpu_enable: 1;

		/** @brief Future use flags currently not supported*/
		uint16_t future_use: 11;

		/** @brief Reserved*/
		uint16_t reserved: 1;
	} power_flags_bitfield;

	struct {
		/** @brief Future use settings currently not supported by SMC*/
		uint16_t future_use[14];
	} power_settings_array;
};

/** @brief Host request to set voltage
 * @details Messages of this type are processed by @ref set_voltage_handler
 */
struct set_voltage_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_SET_VOLTAGE */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief I2C slave address (P0V8_VCORE_ADDR or P0V8_VCOREM_ADDR) */
	uint32_t slave_addr;

	/** @brief Voltage to set in millivolts */
	uint32_t voltage_in_mv;
};

/** @brief Host request to get voltage
 * @details Messages of this type are processed by @ref get_voltage_handler
 */
struct get_voltage_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_GET_VOLTAGE */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief I2C slave address (P0V8_VCORE_ADDR or P0V8_VCOREM_ADDR) */
	uint32_t slave_addr;
};

/** @brief Host request to force the VDD core voltage
 * @details Messages of this type are processed by @ref ForceVddHandler.
 *
 * A forced_voltage of 0 disables forcing and restores the default voltage.
 * Values outside the valid range (VDD_MIN to VDD_MAX) are rejected with
 * return code 1.
 */
struct force_vdd_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_FORCE_VDD */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief The voltage to force in millivolts, or 0 to disable forcing */
	uint32_t forced_voltage;
};

/** @brief Host request to switch VOUT control
 * @details Messages of this type are processed by @ref switch_vout_control_handler
 */
struct switch_vout_control_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_SWITCH_VOUT_CONTROL */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief VOUT control source */
	uint32_t source;
};

/** @brief Host request to switch clock scheme
 * @details Messages of this type are processed by @ref switch_clk_scheme_handler
 */
struct switch_clk_scheme_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_SWITCH_CLK_SCHEME */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief @ref tt_clk_scheme "Clock scheme" to switch to*/
	uint32_t scheme;
};

/** @brief Host request to get frequency curve from voltage
 * @details Requests of this type are processed by @ref get_freq_curve_from_voltage_handler
 */
struct get_freq_curve_from_voltage_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_GET_FREQ_CURVE_FROM_VOLTAGE */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief The input voltage in mV */
	uint32_t input_voltage_mv;
};

/** @brief Host request to get voltage curve from frequency
 * @details Requests of this type are processed by @ref get_voltage_curve_from_freq_handler
 */
struct get_voltage_curve_from_freq_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_GET_VOLTAGE_CURVE_FROM_FREQ */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief The input frequency in MHz */
	uint32_t input_freq_mhz;
};

/** @brief Host request for debug NOC translation
 * @details Messages of this type are processed by @ref debug_noc_translation_handler
 */
struct debug_noc_translation_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_DEBUG_NOC_TRANSLATION */
	uint8_t command_code;

	/** @brief Enable or disable NOC translation*/
	uint8_t enable_translation: 1;

	/** @brief The PCIE instance */
	uint8_t pcie_instance: 1;

	/** @brief Set to 1 to use pcie instance from the  @ref pcie_instance field, or 0 to
	 *         get the pcie instance from FW table
	 */
	uint8_t pcie_instance_override: 1;

	/** @brief Bitmask of bad tensix columns */
	uint16_t bad_tensix_cols;

	/** @brief Instance number of the bad GDDR. 0xFF if all GDDR are good */
	uint8_t bad_gddr;

	/** @brief low byte of skip_eth field*/
	uint8_t skip_eth_low;

	/** @brief hi byte of skip_eth field*/
	uint8_t skip_eth_hi;
};

/** @brief Host request to ping DMC
 * @details Messages of this type are processed by @ref ping_dm_handler.
 * This request is used to test SMBus stability using different transaction types.
 */
struct dmc_ping_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_PING_DM */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief Use legacy ping mode
	 * - true: DMC uses SMBus write word transaction, which could fail without acknowledgment
	 * - false: DMC Uses SMBus read word transaction, which provides DMC acknowledgment
	 */
	bool legacy_ping;
};

/** @brief Host request to set the message queue serial number
 * @details Messages of this type are processed by @ref handle_set_last_serial.
 * This message allows the host to manually set the serial number for message queue
 * synchronization purposes.
 */
struct set_last_serial_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_SET_LAST_SERIAL */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief The serial number to set in the message queue header */
	uint32_t serial_number;
};

/** @brief Host request to send PCIE MSI
 * @details Messages of this type are processed by @ref send_pcie_msi_handler
 */
struct send_pcie_msi_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_SEND_PCIE_MSI */
	uint8_t command_code;

	/** @brief The PCIE instance 0 or 1 */
	uint8_t pcie_inst: 1;

	/** @brief 2 bytes of padding */
	uint8_t pad[2];

	/** @brief MSI vector ID */
	uint32_t vector_id;
};

/** @brief Host request for I2C message transaction
 * @details Messages of this type are processed by @ref i2c_message_handler
 */
struct i2c_message_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_I2C_MESSAGE */
	uint8_t command_code;

	/** @brief I2C master ID */
	uint8_t i2c_mst_id;

	/** @brief I2C slave address (7-bit) */
	uint8_t i2c_slave_address;

	/** @brief Number of bytes to write */
	uint8_t num_write_bytes;

	/** @brief Number of bytes to read */
	uint8_t num_read_bytes;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief Write data buffer (up to 24 bytes) */
	uint8_t write_data[24];
};

/** @brief Host request to blink the LED
 * @details Messages of this type are processed by @ref toggle_blinky_handler.
 *          If is_blinking is high, the LED will blink continuously until stopped.
 *          To stop the blinking, set is_blinking low.
 */
struct led_blink_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_BLINKY */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief Whether LED should be blinking or not.
	 *         Set to 1 to start continuous blinking, 0 to stop blinking.
	 */
	uint8_t is_blinking: 1;
};

/** @brief Host request for test message
 * @details Messages of this type are processed by @ref handle_test
 */
struct test_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_TEST */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief Test input value that will be incremented in the response */
	uint32_t test_value;
};

/** @brief Host request to initiate a PCIe DMA transfer
 * @details Messages of this type are processed by @ref pcie_dma_transfer_handler.
 *
 * For @ref TT_SMC_MSG_PCIE_DMA_HOST_TO_CHIP_TRANSFER the DMA reads from the host
 * and writes to the chip. For @ref TT_SMC_MSG_PCIE_DMA_CHIP_TO_HOST_TRANSFER the
 * DMA reads from the chip and writes to the host.
 *
 * On completion an MSI is sent to the host at msi_completion_addr with
 * the completion_data value.
 */
struct pcie_dma_transfer_rqst {
	/** @brief The command code corresponding to
	 * @ref TT_SMC_MSG_PCIE_DMA_CHIP_TO_HOST_TRANSFER or
	 * @ref TT_SMC_MSG_PCIE_DMA_HOST_TO_CHIP_TRANSFER
	 */
	uint8_t command_code;

	/** @brief Completion data written to the MSI completion address */
	uint8_t completion_data;

	/** @brief Two bytes of padding */
	uint8_t pad[2];

	/** @brief Transfer size in bytes */
	uint32_t transfer_size_bytes;

	/** @brief Chip-side address for the transfer */
	uint64_t chip_addr;

	/** @brief Host-side address for the transfer */
	uint64_t host_addr;

	/** @brief MSI completion address on the host */
	uint64_t msi_completion_addr;
};

/** @brief Host request to reset a single Tensix tile
 * @details Messages of this type are processed by @ref ToggleSingleTensixReset.
 *
 * On success, response data[0] = 0.
 * Returns non-zero on failure.
 */
struct toggle_single_tensix_reset_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_TOGGLE_SINGLE_TENSIX_RESET */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief NOC0 X coordinate of the target tile */
	uint8_t noc_x;

	/** @brief NOC0 Y coordinate of the target tile */
	uint8_t noc_y;
};

/** @brief Host request to toggle GDDR reset
 * @details Messages of this type are processed by @ref toggle_gddr_reset.
 *
 * On failure response data[1] contains one of the @ref gddr_reset_err values.
 */
struct gddr_reset_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_TOGGLE_GDDR_RESET */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief GDDR controller instance (0-7) */
	uint32_t gddr_inst;
};

/** @brief Error codes returned in response data[1] for TT_SMC_MSG_TOGGLE_GDDR_RESET */
enum gddr_reset_err {
	GDDR_RESET_ERR_INVALID_INST = 1,
	GDDR_RESET_ERR_HARVESTED = 2,
	GDDR_RESET_ERR_MASKED = 3,
	GDDR_RESET_ERR_TRAINING = 4,
	GDDR_RESET_ERR_BIST = 5,
	GDDR_RESET_ERR_POWERDOWN = 6,
};

/** @brief Host request to reset and reinitialize one or more ETH tiles
 *
 * The handler performs the appropriate sequencing to reset and (optionally)
 * restart ERISC FW on the requested ethernet tiles.
 *
 * Request: @c eth_inst_mask is a bitmask of ETH instance indices (bit N selects ETH N).
 * Bits for harvested instances are silently dropped. An empty effective mask returns success
 * (no-op). When @c no_fw_reload is 1, SPI FW/cfg reload and @c ReleaseEthReset are skipped.
 *
 * Wire layout as @c uint32_t data[8]: @c data[1] = @c eth_inst_mask; @c data[2] bit 0 =
 * @c no_fw_reload.
 *
 * On failure, response @c data[0] exit code is 1 @c data[1] contains @ref eth_reset_err.
 * On success, response @c data[0] exit code is 0 and @c data[1] is the bitmask of the
 * successfully reset tiles.
 */
struct eth_tile_reset_rqst {
	/** @brief Command code @ref TT_SMC_MSG_TOGGLE_ETH_RESET */
	uint8_t command_code;

	/** @brief Padding */
	uint8_t pad[3];

	/** @brief Bitmask of ETH instances to reset (bit N -> ETH N) */
	uint32_t eth_inst_mask;

	/** @brief If 1, tile/RISC reset only; skip SPI FW/cfg reload and ReleaseEthReset */
	uint8_t no_fw_reload: 1;
};

/** @brief Error codes in response data[1] for @ref TT_SMC_MSG_TOGGLE_ETH_RESET */
enum eth_reset_err {
	ETH_RESET_ERR_INVALID_MASK = 1,
	ETH_RESET_ERR_CABLE_FAULT = 2,
	ETH_RESET_ERR_NO_FLASH = 3,
	ETH_RESET_ERR_FW_LOOKUP = 4,
	ETH_RESET_ERR_FW_LOAD = 5,
	ETH_RESET_ERR_CFG_LOOKUP = 6,
	ETH_RESET_ERR_CFG_SIZE = 7,
	ETH_RESET_ERR_CFG_LOAD = 8,
};

/** @brief Host request to trigger a chip reset
 * @details Messages of this type are processed by @ref reset_dm_handler.
 *
 * The reset is delayed by 5 ms to allow the response to be sent before
 * the reset occurs. Invalid reset levels are rejected with the level
 * returned as the error code.
 */
struct trigger_reset_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_TRIGGER_RESET */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief Reset level: 0 = ASIC-only reset, 3 = ASIC + M3 (DMC) reset */
	uint32_t reset_level;
};

/** @brief Host request to change ASIC state
 * @details Messages of this type are processed by @ref asic_state_handler.
 */
struct asic_state_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_ASIC_STATE0 or @ref
	 * TT_SMC_MSG_ASIC_STATE3
	 * @details TT_SMC_MSG_ASIC_STATE0 transitions to @ref A0State.
	 *          TT_SMC_MSG_ASIC_STATE3 transitions to @ref A3State.
	 */
	uint8_t command_code;
};

/** @brief Host request to read a temperature sensor
 * @details Messages of this type are processed by @ref read_ts_handler.
 *
 * On success, response data[1] contains the raw sensor reading and
 * data[2] contains the temperature in telemetry format.
 */
struct read_ts_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_READ_TS */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief Temperature sensor instance ID */
	uint32_t sensor_id;
};

/** @brief Host request to read or write SPI EEPROM
 * @details Read messages are processed by @ref read_eeprom_handler.
 *          Write messages are processed by @ref write_eeprom_handler.
 *
 * The host must use flash unlock (@ref TT_SMC_MSG_FLASH_UNLOCK) before writing.
 * The CSM buffer address must fall within the SPI global buffer region.
 */
struct eeprom_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_READ_EEPROM or
	 * @ref TT_SMC_MSG_WRITE_EEPROM
	 */
	uint8_t command_code;

	/** @brief Buffer memory type (0 = CSM scratch buffer) */
	uint8_t buffer_mem_type;

	/** @brief Two bytes of padding */
	uint8_t pad[2];

	/** @brief SPI flash address to read from or write to */
	uint32_t spi_address;

	/** @brief Number of bytes to transfer */
	uint32_t num_bytes;

	/** @brief CSM buffer address for the data */
	uint32_t csm_addr;
};

/** @brief Host request to read a process detector
 * @details Messages of this type are processed by @ref read_pd_handler.
 *
 * On success, response data[1] contains the raw sensor reading and
 * data[2] contains the frequency in telemetry format.
 */
struct read_pd_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_READ_PD */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief Delay chain selection */
	uint32_t delay_chain;

	/** @brief Process detector instance ID */
	uint32_t sensor_id;
};

/** @brief Host request to read a voltage monitor
 * @details Messages of this type are processed by @ref read_vm_handler.
 *
 * On success, response data[1] contains the raw sensor reading and
 * data[2] contains the voltage in millivolts.
 */
struct read_vm_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_READ_VM */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief Voltage monitor instance ID */
	uint32_t sensor_id;
};

/** @brief Host request to set the TDP limit
 * @details Messages of this type are processed by @ref set_tdp_limit_handler
 */
struct set_tdp_limit_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_SET_TDP_LIMIT */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief The TDP limit to set in watts */
	uint32_t tdp_limit;

	/** @brief Restore the TDP limit to the default value */
	uint8_t restore_default: 1;
};

/** @brief Host request to report scratch-only status
 * @details Messages of this type are processed by @ref process_queued_message.
 * This is a command-only message with no payload fields.
 *
 * The response data[0] is set to @ref MESSAGE_QUEUE_STATUS_SCRATCH_ONLY (0xfe).
 */
struct report_scratch_only_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_REPORT_SCRATCH_ONLY */
	uint8_t command_code;
};

/** @brief Host request to set the ASIC fmax
 * @details Messages of this type are processed by @ref set_arb_host_fmax_handler
 */
struct set_asic_host_fmax_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_SET_ASIC_HOST_FMAX */
	uint8_t command_code;
	/** @brief Three bytes of padding */
	uint8_t pad[3];
	/** @brief The ASIC host fmax to set in MHz */
	uint32_t asic_fmax;
	/** @brief Restore the ASIC host fmax to the default value */
	uint8_t restore_default: 1;
};

/** @brief Characterization submessage to set host-requested minimum frequency floor
 * @details Payload is a single uint32_t with two interpretations:
 * - Value == 1: Restore default (disable host fmin floor)
 * - Value in [200, 1400]: Set frequency to this value in MHz
 */
struct characterisation_set_fmin_submsg {
	/** @brief Either 1 (restore) or frequency in MHz (200-1400) */
	uint32_t value;
};

/** @brief Union of all possible characterization submessage payloads */
union characterisation_submsg_data {
	/** @brief Set host-requested minimum frequency floor */
	struct characterisation_set_fmin_submsg fmin_value;
	/* add to this union to define more sub-message payloads */
	/** @brief Generic fallback for raw access */
	uint8_t raw_data[4];
};

/** @brief Generic characterization message for internal SMC use
 * @warning This is an internal interface. Direct use is not recommended
 *          unless implementing new characterization features.
 * @details Uses submsg_ID to dispatch to specific operations.
 *          Messages of this type are processed by @ref characterisation_handler
 */
struct characterisation_msg_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_CHARACTERISATION */
	uint8_t command_code;
	/** @brief Submessage ID identifying which payload structure is active */
	uint8_t submsg_ID;
	/** @brief Two bytes of padding */
	uint8_t pad[2];
	/** @brief The submessage payload (interpretation depends on submsg_ID) */
	union characterisation_submsg_data submsg_data;
};

/** @brief Host request to reinitialize Tensix NOC programming and tile configuration
 * @details Messages of this type are processed by @ref ReinitTensix.
 *
 * This is a command-only request with no additional arguments.
 */
struct reinit_tensix_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_REINIT_TENSIX */
	uint8_t command_code;
};

/** @brief Host request to toggle Tensix reset
 * @details Messages of this type are processed by @ref ToggleTensixReset.
 *
 * This is a command-only request with no additional arguments.
 */
struct toggle_tensix_reset_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_TOGGLE_TENSIX_RESET */
	uint8_t command_code;
};

/** @brief Host request to unlock flash for writing
 * @details Messages of this type are processed by @ref flash_unlock_handler.
 *
 * This is a command-only request with no additional arguments.
 */
struct flash_unlock_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_FLASH_UNLOCK */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];
};

/** @brief Host request to set watchdog timeout
 * @details Messages of this type are processed by @ref set_watchdog_timeout.
 */
struct set_wdt_timeout_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_SET_WDT_TIMEOUT */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief The watchdog timeout value in milliseconds
	 *
	 * Valid values:
	 * - 0: Disable the watchdog timer completely
	 * - >CONFIG_TT_BH_ARC_WDT_FEED_INTERVAL: Enable watchdog with specified timeout
	 *
	 * Values between 1 and CONFIG_TT_BH_ARC_WDT_FEED_INTERVAL (inclusive) are
	 * rejected with ENOTSUP, as they are below the minimum feed interval.
	 */
	uint32_t timeout_ms;
};

/** @brief Host request to lock flash writes
 * @details Messages of this type are processed by @ref flash_lock_handler.
 *
 * This is a command-only request with no additional arguments.
 */
struct flash_lock_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_FLASH_LOCK */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];
};

/** @brief Host request to confirm SPI flash operation
 * @details Messages of this type are processed by @ref confirm_flashed_spi_handler.
 *
 * Challenge message issued from tt-flash to confirm a firmware update.
 */
struct confirm_flashed_spi_rqst {
	/** @brief The command code corresponding to @ref TT_SMC_MSG_CONFIRM_FLASHED_SPI */
	uint8_t command_code;

	/** @brief Three bytes of padding */
	uint8_t pad[3];

	/** @brief Challenge data for confirmation */
	uint32_t challenge_data;
};

/** @brief A tenstorrent host request*/
union request {
	/** @brief The interpretation of the request as an array of uint32_t entries*/
	uint32_t data[REQUEST_MSG_LEN];

	/** @brief The interpretation of the request as just the first byte representing command
	 * code
	 */
	uint8_t command_code;

	/** @brief A force fan speed request*/
	struct force_fan_speed_rqst force_fan_speed;

	/** @brief An AICLK set speed request*/
	struct aiclk_set_speed_rqst aiclk_set_speed;

	/** @brief An AICLK sweep start or stop request */
	struct aisweep_rqst aisweep;

	/** @brief A force AICLK frequency request */
	struct force_aiclk_rqst force_aiclk;

	/** @brief A get AICLK frequency request */
	struct get_aiclk_rqst get_aiclk;

	/** @brief A power setting request*/
	struct power_setting_rqst power_setting;

	/** @brief A set voltage request */
	struct set_voltage_rqst set_voltage;

	/** @brief A get voltage request */
	struct get_voltage_rqst get_voltage;

	/** @brief A force VDD voltage request */
	struct force_vdd_rqst force_vdd;

	/** @brief A switch VOUT control request */
	struct switch_vout_control_rqst switch_vout_control;

	/** @brief A switch clock scheme request */
	struct switch_clk_scheme_rqst switch_clk_scheme;

	/** @brief A get frequency curve from voltage request */
	struct get_freq_curve_from_voltage_rqst get_freq_curve_from_voltage;

	/** @brief A get voltage curve from frequency request */
	struct get_voltage_curve_from_freq_rqst get_voltage_curve_from_freq;

	/** @brief A debug NOC translation request */
	struct debug_noc_translation_rqst debug_noc_translation;

	/** @brief A dmc ping request */
	struct dmc_ping_rqst dmc_ping;

	/** @brief A set last serial request */
	struct set_last_serial_rqst set_last_serial;

	/** @brief A Send PCIE MSI request */
	struct send_pcie_msi_rqst send_pci_msi;

	/** @brief An I2C message request */
	struct i2c_message_rqst i2c_message;

	/** @brief The led blinking request */
	struct led_blink_rqst blink;

	/** @brief A test request */
	struct test_rqst test;

	/** @brief A PCIe DMA transfer request */
	struct pcie_dma_transfer_rqst pcie_dma_transfer;

	/** @brief An ASIC state transition request */
	struct asic_state_rqst asic_state;

	/** @brief A trigger reset request */
	struct trigger_reset_rqst trigger_reset;
	/** @brief A single Tensix reset request */
	struct toggle_single_tensix_reset_rqst toggle_single_tensix_reset;

	/** @brief A GDDR reset request */
	struct gddr_reset_rqst gddr_reset;

	/** @brief An ETH tile reset request */
	struct eth_tile_reset_rqst eth_tile_reset;

	/** @brief A temperature sensor read request */
	struct read_ts_rqst read_ts;

	/** @brief A voltage monitor read request */
	struct read_vm_rqst read_vm;

	/** @brief A process detector read request */
	struct read_pd_rqst read_pd;

	/** @brief A set TDP limit request */
	struct set_tdp_limit_rqst set_tdp_limit;

	/** @brief An EEPROM read or write request */
	struct eeprom_rqst eeprom;

	/** @brief A set ASIC host fmax request */
	struct set_asic_host_fmax_rqst set_asic_host_fmax;

	/** @brief A set ASIC host fmin request */
	struct characterisation_msg_rqst characterisation_msg;

	/** @brief A report scratch-only request */
	struct report_scratch_only_rqst report_scratch_only;

	/** @brief A generic counter request */
	struct counter_rqst counter;

	/** @brief A set watchdog timeout request */
	struct set_wdt_timeout_rqst set_wdt_timeout;

	/** @brief A flash unlock request */
	struct flash_unlock_rqst flash_unlock;

	/** @brief A flash lock request */
	struct flash_lock_rqst flash_lock;

	/** @brief A confirm SPI flash request */
	struct confirm_flashed_spi_rqst confirm_flashed_spi;
};

/** @} */

/** @} */

struct response {
	uint32_t data[RESPONSE_MSG_LEN];
};

typedef uint8_t (*msgqueue_request_handler_t)(const union request *req, struct response *rsp);

struct msgqueue_handler {
	uint32_t msg_type;
	msgqueue_request_handler_t handler;
};

#define REGISTER_MESSAGE(msg, func)                                                                \
	const STRUCT_SECTION_ITERABLE(msgqueue_handler, registration_for_##msg) = {                \
		.msg_type = msg,                                                                   \
		.handler = func,                                                                   \
	}

void process_message_queues(void);
void msgqueue_register_handler(uint32_t msg_code, msgqueue_request_handler_t handler);

int msgqueue_request_push(uint32_t msgqueue_id, const union request *request);
int msgqueue_request_pop(uint32_t msgqueue_id, union request *request);
int msgqueue_response_push(uint32_t msgqueue_id, const struct response *response);
int msgqueue_response_pop(uint32_t msgqueue_id, struct response *response);
void init_msgqueue(void);

#ifdef __cplusplus
}
#endif

#endif
