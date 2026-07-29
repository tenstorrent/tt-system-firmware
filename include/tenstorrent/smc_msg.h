/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file smc_msg.h
 * @brief Tenstorrent host command IDs
 */

#ifndef TT_SMC_MSG__H_
#define TT_SMC_MSG__H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup tt_smc_msg_enum SMC Message IDs
 * @ingroup tt_msg_apis
 * @brief Enumeration of all available SMC message command IDs
 * @{
 */

/** @brief Enumeration listing the available host requests IDs the SMC can process*/
enum tt_smc_msg {
	/** @brief @ref set_voltage_rqst "Set voltage request" */
	TT_SMC_MSG_SET_VOLTAGE = 0x12,

	/** @brief @ref get_voltage_rqst "Get voltage request" */
	TT_SMC_MSG_GET_VOLTAGE = 0x13,

	/** @brief @ref switch_clk_scheme_rqst "Switch clock scheme request" */
	TT_SMC_MSG_SWITCH_CLK_SCHEME = 0x14,

	/** @brief @ref debug_noc_translation_rqst "Debug NOC translation request" */
	TT_SMC_MSG_DEBUG_NOC_TRANSLATION = 0x15,

	/** @brief @ref report_scratch_only_rqst "Report scratch-only request" */
	TT_SMC_MSG_REPORT_SCRATCH_ONLY = 0x16,

	/** @brief @ref send_pcie_msi_rqst "Send PCIe MSI request" */
	TT_SMC_MSG_SEND_PCIE_MSI = 0x17,

	/** @brief @ref switch_vout_control_rqst "Switch VOUT control request" */
	TT_SMC_MSG_SWITCH_VOUT_CONTROL = 0x18,

	/** @brief @ref eeprom_rqst "Read SPI EEPROM request" */
	TT_SMC_MSG_READ_EEPROM = 0x19,

	/** @brief @ref eeprom_rqst "Write SPI EEPROM request" */
	TT_SMC_MSG_WRITE_EEPROM = 0x1A,

	/** @brief @ref read_ts_rqst "Read temperature sensor request" */
	TT_SMC_MSG_READ_TS = 0x1B,

	/** @brief @ref read_pd_rqst "Read process detector request" */
	TT_SMC_MSG_READ_PD = 0x1C,

	/** @brief @ref read_vm_rqst "Read voltage monitor request" */
	TT_SMC_MSG_READ_VM = 0x1D,

	/** @brief @ref i2c_message_rqst "I2C message request" */
	TT_SMC_MSG_I2C_MESSAGE = 0x1E,

	/** @brief @ref reinit_tensix_rqst "Reinitialize Tensix request" */
	TT_SMC_MSG_REINIT_TENSIX = 0x20,

	/** @brief @ref power_setting_rqst "Power Setting Request"*/
	TT_SMC_MSG_POWER_SETTING = 0x21,

	/** @brief @ref set_tdp_limit_rqst "Set TDP limit request" */
	TT_SMC_MSG_SET_TDP_LIMIT = 0x22,

	/** @brief @ref set_asic_host_fmax_rqst "Set ASIC fmax request" */
	TT_SMC_MSG_SET_ASIC_HOST_FMAX = 0x23,

	/** @brief @ref get_freq_curve_from_voltage_rqst "Frequency Curve from Voltage Request"*/
	TT_SMC_MSG_GET_FREQ_CURVE_FROM_VOLTAGE = 0x30,

	/** @brief @ref aisweep_rqst "Start AICLK sweep request" */
	TT_SMC_MSG_AISWEEP_START = 0x31,

	/** @brief @ref aisweep_rqst "Stop AICLK sweep request" */
	TT_SMC_MSG_AISWEEP_STOP = 0x32,

	/** @brief @ref force_aiclk_rqst "Force AICLK frequency request" */
	TT_SMC_MSG_FORCE_AICLK = 0x33,

	/** @brief @ref get_aiclk_rqst "Get AICLK frequency request" */
	TT_SMC_MSG_GET_AICLK = 0x34,

	/** @brief @ref counter_rqst "Generic Counter Request" */
	TT_SMC_MSG_COUNTER = 0x35,

	/** @brief @ref force_vdd_rqst "Force VDD voltage request" */
	TT_SMC_MSG_FORCE_VDD = 0x39,

	/** @brief @ref aiclk_set_speed_rqst "AI Clock Set Busy Speed Request"*/
	TT_SMC_MSG_AICLK_GO_BUSY = 0x52,

	/** @brief @ref aiclk_set_speed_rqst "AI Clock Set Idle Speed Request"*/
	TT_SMC_MSG_AICLK_GO_LONG_IDLE = 0x54,

	/** @brief @ref trigger_reset_rqst "Trigger chip reset request" */
	TT_SMC_MSG_TRIGGER_RESET = 0x56,

	/** @brief @ref test_rqst "Test request" */
	TT_SMC_MSG_TEST = 0x90,

	/** @brief @ref pcie_dma_transfer_rqst "PCIe DMA chip-to-host transfer request" */
	TT_SMC_MSG_PCIE_DMA_CHIP_TO_HOST_TRANSFER = 0x9B,

	/** @brief @ref pcie_dma_transfer_rqst "PCIe DMA host-to-chip transfer request" */
	TT_SMC_MSG_PCIE_DMA_HOST_TO_CHIP_TRANSFER = 0x9C,

	/** @brief @ref asic_state_rqst "ASIC State 0 (A0State) request" */
	TT_SMC_MSG_ASIC_STATE0 = 0xA0,

	/** @brief @ref asic_state_rqst "ASIC State 3 (A3State) request" */
	TT_SMC_MSG_ASIC_STATE3 = 0xA3,

	/** @brief @ref get_voltage_curve_from_freq_rqst "Voltage Curve from Frequency Request"*/
	TT_SMC_MSG_GET_VOLTAGE_CURVE_FROM_FREQ = 0xA6,

	/** @brief @ref force_fan_speed_rqst "Force Fan Speed Request"*/
	TT_SMC_MSG_FORCE_FAN_SPEED = 0xAC,

	/** @brief Get DRAM temperature request (not supported) */
	TT_SMC_MSG_GET_DRAM_TEMPERATURE = 0xAD,

	/** @brief @ref toggle_single_tensix_reset_rqst "Toggle single Tensix reset request" */
	TT_SMC_MSG_TOGGLE_SINGLE_TENSIX_RESET = 0xAE,

	/** @brief @ref toggle_tensix_reset_rqst "Toggle Tensix reset request" */
	TT_SMC_MSG_TOGGLE_TENSIX_RESET = 0xAF,

	/** @brief @ref eth_tile_reset_rqst "Toggle ETH tile reset request" */
	TT_SMC_MSG_TOGGLE_ETH_RESET = 0xB0,

	/** @brief @ref gddr_reset_rqst "Toggle GDDR reset request" */
	TT_SMC_MSG_TOGGLE_GDDR_RESET = 0xB6,

	/** @brief @ref set_last_serial_rqst "Set message queue serial number request" */
	TT_SMC_MSG_SET_LAST_SERIAL = 0xBE,

	/** @brief @ref dmc_ping_rqst "Ping DMC request" */
	TT_SMC_MSG_PING_DM = 0xC0,

	/** @brief @ref set_wdt_timeout_rqst "Set watchdog timeout request" */
	TT_SMC_MSG_SET_WDT_TIMEOUT = 0xC1,

	/** @brief @ref flash_unlock_rqst "Flash write unlock request" */
	TT_SMC_MSG_FLASH_UNLOCK = 0xC2,

	/** @brief @ref flash_lock_rqst "Flash write lock request" */
	TT_SMC_MSG_FLASH_LOCK = 0xC3,

	/** @brief @ref confirm_flashed_spi_rqst "Confirm SPI flash succeeded" */
	TT_SMC_MSG_CONFIRM_FLASHED_SPI = 0xC4,

	/** @brief @ref led_blink_rqst "Toggle red LED on the board" */
	TT_SMC_MSG_BLINKY = 0xC5,

	/** @brief @ref characterisation_rqst "Generic characterization message" */
	TT_SMC_MSG_CHARACTERISATION = 0xC6,

	/** @brief @ref tt_pcie_log_rqst "FW logging setup/release request" */
	TT_SMC_MSG_TT_PCIE_LOG = 0xC7,
};

/** @brief Enumeration of characterization submessage IDs */
enum char_submsg_ids {
	/** @brief Set host-requested minimum frequency (AICLK) */
	TT_SUB_MSG_SET_HOST_REQUESTED_FMIN = 0x1,
	/** @brief Enable/disable kernel throttler at AICLK floor */
	TT_SUB_MSG_SET_KERNEL_THROTTLER_ENABLED = 0x2,
	/** @brief Set frequency limit for stopping kernel throttler */
	TT_SUB_MSG_SET_KERNEL_THROTTLER_STOP_NOPS_FREQ = 0x3,
};

/** @} */

#ifdef __cplusplus
}
#endif

#endif
