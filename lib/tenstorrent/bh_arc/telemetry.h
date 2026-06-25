/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @defgroup telemetry Telemetry
 * @brief Telemetry definitions and functions
 *
 * @ref telemetry_tags "Telemetry Tags" describes the various telemetry indices the SMC firmawre
 * currently supports. @ref telemetry_table "Telemetry Table" provides telemetry tracking
 * infrastructure related information.
 *
 */

/** @brief  The current version of the tenstorrent telemetry interface
 * v0.1.0 - Only update when redefining the meaning of an existing tag
 * Semver format: 0 x 00 Major Minor Patch
 */
#define TELEMETRY_VERSION 0x00000100

/**
 * @defgroup telemetry_tags Telemetry Tags
 * @ingroup telemetry
 * @brief Telemetry tag definitions
 * @{
 */

/** @brief High part of the board ID. */
#define TAG_BOARD_ID_HIGH 1

/** @brief Low part of the board ID. */
#define TAG_BOARD_ID_LOW 2

/** @brief ASIC ID. */
#define TAG_ASIC_ID 3

/** @brief Harvesting state of the system. */
#define TAG_HARVESTING_STATE 4

/** @brief Update interval for telemetry in milliseconds. */
#define TAG_UPDATE_TELEM_SPEED 5

/** @brief VCore voltage in millivolts. */
#define TAG_VCORE 6

/** @brief Thermal design power (TDP) in watts. */
#define TAG_TDP 7

/** @brief Thermal design current (TDC) in amperes. */
#define TAG_TDC 8

/** @brief VDD limits (min and max) in millivolts. */
#define TAG_VDD_LIMITS 9

/** @brief Thermal shutdown limit in degrees Celsius. */
#define TAG_THM_LIMIT_SHUTDOWN 10

/** @brief ASIC temperature in signed 16.16 fixed-point format. */
#define TAG_ASIC_TEMPERATURE 11

/** @brief Voltage regulator temperature in degrees Celsius. (Not implemented) */
#define TAG_VREG_TEMPERATURE 12

/** @brief Board temperature in degrees Celsius. (Not implemented) */
#define TAG_BOARD_TEMPERATURE 13

/** @brief AI clock frequency in megahertz. */
#define TAG_AICLK 14

/** @brief AXI clock frequency in megahertz. */
#define TAG_AXICLK 15

/** @brief ARC clock frequency in megahertz. */
#define TAG_ARCCLK 16

/** @brief L2CPU clock 0 frequency in megahertz. */
#define TAG_L2CPUCLK0 17

/** @brief L2CPU clock 1 frequency in megahertz. */
#define TAG_L2CPUCLK1 18

/** @brief L2CPU clock 2 frequency in megahertz. */
#define TAG_L2CPUCLK2 19

/** @brief L2CPU clock 3 frequency in megahertz. */
#define TAG_L2CPUCLK3 20

/** @brief Ethernet live status.
 * [15:0] - heartbeat status bitmask
 *      (1 = heartbeat alive, 0 = no heartbeat)
 * [31:16] - live link status bitmask
 *      (1 = link up, 0 = link down)
 */
#define TAG_ETH_LIVE_STATUS 21

/** @brief GDDR training and BIST status.
 * [0] - Training complete GDDR 0
 * [1] - Error GDDR 0
 * [2] - Training complete GDDR 1
 * [3] - Error GDDR 1
 * ...
 * [14] - Training Complete GDDR 7
 * [15] - Error GDDR 7
 * [16] - BIST complete GDDR 0
 * [17] - BIST failed GDDR 0
 * [18] - BIST complete GDDR 1
 * [19] - BIST failed GDDR 1
 * ...
 * [30] - BIST complete GDDR 7
 * [31] - BIST failed GDDR 7
 */
#define TAG_GDDR_STATUS 22

/** @brief GDDR speed in megabits per second. */
#define TAG_GDDR_SPEED 23

/** @brief Ethernet firmware version. */
#define TAG_ETH_FW_VERSION 24

/** @brief GDDR firmware version. */
#define TAG_GDDR_FW_VERSION 25

/** @brief DM application firmware version. */
#define TAG_DM_APP_FW_VERSION 26

/** @brief DM bootloader firmware version. */
#define TAG_DM_BL_FW_VERSION 27

/** @brief Flash bundle version. */
#define TAG_FLASH_BUNDLE_VERSION 28

/** @brief CM firmware version. */
#define TAG_CM_FW_VERSION 29

/** @brief L2CPU firmware version. */
#define TAG_L2CPU_FW_VERSION 30

/** @brief Fan speed as a percentage. 0xFFFFFFFF if fan control disabled. */
#define TAG_FAN_SPEED 31

/** @brief Timer heartbeat counter. */
#define TAG_TIMER_HEARTBEAT 32

/** @brief Total number of telemetry tags. */
#define TAG_TELEM_ENUM_COUNT 33

/** @brief Enabled Tensix columns. */
#define TAG_ENABLED_TENSIX_COL 34

/** @brief Enabled Ethernet interfaces. */
#define TAG_ENABLED_ETH 35

/** @brief Enabled GDDR interfaces. */
#define TAG_ENABLED_GDDR 36

/** @brief Enabled L2CPU cores. */
#define TAG_ENABLED_L2CPU 37

/** @brief PCIe usage information. */
#define TAG_PCIE_USAGE 38

/** @brief Input current in amperes. */
#define TAG_INPUT_CURRENT 39

/** @brief NOC translation status. */
#define TAG_NOC_TRANSLATION 40

/** @brief Fan RPM. 0xFFFFFFFF if fan control disabled. */
#define TAG_FAN_RPM 41

/** @brief GDDR 0 and 1 temperature.
 *
 * Each die temperature is one byte in degrees Celsius, packed as:
 * - [31:24] GDDR 1 top die
 * - [23:16] GDDR 1 bottom die
 * - [15:8]  GDDR 0 top die
 * - [7:0]   GDDR 0 bottom die
 */
#define TAG_GDDR_0_1_TEMP 42

/** @brief GDDR 2 and 3 temperature.
 *
 * Each die temperature is one byte in degrees Celsius, packed as:
 * - [31:24] GDDR 3 top die
 * - [23:16] GDDR 3 bottom die
 * - [15:8]  GDDR 2 top die
 * - [7:0]   GDDR 2 bottom die
 */
#define TAG_GDDR_2_3_TEMP 43

/** @brief GDDR 4 and 5 temperature.
 *
 * Each die temperature is one byte in degrees Celsius, packed as:
 * - [31:24] GDDR 5 top die
 * - [23:16] GDDR 5 bottom die
 * - [15:8]  GDDR 4 top die
 * - [7:0]   GDDR 4 bottom die
 */
#define TAG_GDDR_4_5_TEMP 44

/** @brief GDDR 6 and 7 temperature.
 *
 * Each die temperature is one byte in degrees Celsius, packed as:
 * - [31:24] GDDR 7 top die
 * - [23:16] GDDR 7 bottom die
 * - [15:8]  GDDR 6 top die
 * - [7:0]   GDDR 6 bottom die
 */
#define TAG_GDDR_6_7_TEMP 45

/** @brief GDDR 0 and 1 corrected errors. */
#define TAG_GDDR_0_1_CORR_ERRS 46

/** @brief GDDR 2 and 3 corrected errors. */
#define TAG_GDDR_2_3_CORR_ERRS 47

/** @brief GDDR 4 and 5 corrected errors. */
#define TAG_GDDR_4_5_CORR_ERRS 48

/** @brief GDDR 6 and 7 corrected errors. */
#define TAG_GDDR_6_7_CORR_ERRS 49

/** @brief GDDR uncorrected errors. */
#define TAG_GDDR_UNCORR_ERRS 50

/** @brief Maximum GDDR temperature. */
#define TAG_MAX_GDDR_TEMP 51

/** @brief ASIC location. */
#define TAG_ASIC_LOCATION 52

/** @brief Board power limit in watts. */
#define TAG_BOARD_POWER_LIMIT 53

/** @brief Input power in watts. */
#define TAG_INPUT_POWER 54

/** @brief Maximum TDC limit in amperes. */
#define TAG_TDC_LIMIT_MAX 55

/** @brief Thermal throttle limit in degrees Celsius. */
#define TAG_THM_LIMIT_THROTTLE 56

/** @brief Firmware build date. */
#define TAG_FW_BUILD_DATE 57

/** @brief TT flash version. */
#define TAG_TT_FLASH_VERSION 58

/** @brief Enabled Tensix rows. */
#define TAG_ENABLED_TENSIX_ROW 59

/** @brief Thermal trip count. */
#define TAG_THERM_TRIP_COUNT 60

/** @brief High part of the ASIC ID. */
#define TAG_ASIC_ID_HIGH 61

/** @brief Low part of the ASIC ID. */
#define TAG_ASIC_ID_LOW 62

/** @brief Maximum AI clock frequency. */
#define TAG_AICLK_LIMIT_MAX 63

/** @brief Maximum TDP limit in watts. */
#define TAG_TDP_LIMIT_MAX 64

/**
 * @brief Effective minimum AICLK arbiter value in megahertz, and the effective arbiter
 *
 * The telemetry value contains both the frequency and the arbiter:
 * - Lower 16 bits: frequency in MHz
 * - Upper 16 bits: arbiter number from @ref aiclk_arb_min
 *
 * The frequency represents the highest frequency requested by all enabled minimum arbiters.
 * Multiple arbiters may request minimum frequencies, and the highest value is effective.
 *
 * @see @ref aiclk_arb_min
 */
#define TAG_AICLK_ARB_MIN 65

/**
 * @brief Effective maximum AICLK arbiter value in megahertz, and the effective arbiter.
 *
 * The telemetry value contains both the frequency and the arbiter:
 * - Lower 16 bits: frequency in MHz
 * - Upper 16 bits: arbiter number from @ref aiclk_arb_max
 *
 * The frequency represents the lowest frequency limit imposed by all enabled maximum arbiters.
 * Multiple arbiters may impose maximum frequency limits (e.g., TDP, TDC, thermal throttling),
 * and the lowest (most restrictive) value is effective. This value takes precedence over
 * TAG_AICLK_ARB_MIN when determining the final target frequency.
 *
 * @see @ref aiclk_arb_max
 */
#define TAG_AICLK_ARB_MAX 66

/**
 * @brief Bitmask of enabled minimum arbiters.
 *
 * Each bit represents whether a specific minimum frequency arbiter is currently enabled.
 * Bit positions correspond to the values in @ref aiclk_arb_min.
 *
 * @see @ref aiclk_arb_min
 */
#define TAG_ENABLED_MIN_ARB 67

/**
 * @brief Bitmask of enabled maximum arbiters.
 *
 * Each bit represents whether a specific maximum frequency arbiter is currently enabled.
 * Bit positions correspond to the values in @ref aiclk_arb_max.
 *
 * @see @ref aiclk_arb_max
 */
#define TAG_ENABLED_MAX_ARB 68

/**
 * @brief Information on the currently set aiclk value
 *
 * Contains details about why a particular target frequency was chosen,
 * including which arbiter determined the frequency and the reason for the decision.
 *
 * @see @ref aiclk_targ_freq_info
 */
#define TAG_AICLK_PPM_INFO 69

/**
 * @brief Host-adjustable AICLK fmax ceiling in megahertz.
 *
 * This tag reflects the value set by the TT_SMC_MSG_SET_ASIC_HOST_FMAX message.
 * A value of 0 indicates the host has not set a limit.
 *
 * @see @ref set_asic_host_fmax_rqst
 */
#define TAG_HOST_AICLK_LIMIT 70

/** @brief Number of SMBUS target errors observed. */
#define TAG_SMBUS_ERRORS 71

/**
 * @brief noc2axi port MRISC FW is loaded on for each GDDR instance.
 *
 * Each nibble represents the noc2axi port (0, 1 or 2) that MRISC FW is loaded on
 * for a GDDR instance. Nibble i corresponds to GDDR instance i, with instance 0
 * in the least significant nibble. A nibble value of 0xF indicates the instance is
 * disabled/harvested.
 */
#define TAG_GDDR_MRISC_NOC2AXI_PORT 72

/**
 * @brief GDDR west IO rail power in watts.
 *
 * Derived from the west IO rail current and the fixed 1.35 V rail supply.
 */
#define TAG_GDDR_WEST_IO_POWER 73

/**
 * @brief GDDR east IO rail power in watts.
 *
 * Derived from the east IO rail current and the fixed 1.35 V rail supply.
 */
#define TAG_GDDR_EAST_IO_POWER 74

/**
 * @brief Kernel-throttler-at-AICLK-floor configuration.
 *
 * Reflects the persisted firmware-table configuration (which may be overridden
 * via the SPI-flash A/B ccfgovr mechanism):
 * - bit 0: feature enabled (1) or disabled (0)
 * - bits [31:16]: stop-NOPs frequency in MHz (0 = FW-controlled default)
 */
#define TAG_KERNEL_THROTTLER 75

/** @brief Number of times kernel NOP throttling has started.
 *
 * Counts the transitions into the throttled (NOP) state since boot, derived
 * from the throttler's transition counter. Monotonically increasing.
 */
#define TAG_NOP_START_COUNT 76

/** @} */ /* end of telemetry_tag group */

/* Not a real tag, signifies the last tag in the list.
 * MUST be incremented if new tags are defined.
 */
#define TAG_COUNT 77

/* Telemetry tags are at offset `tag` in the telemetry buffer */
#define TELEM_OFFSET(tag) (tag)

void init_telemetry(uint32_t app_version);
uint32_t ConvertFloatToTelemetry(float value);
float ConvertTelemetryToFloat(int32_t value);
void StartTelemetryTimer(void);
void UpdateDmFwVersion(uint32_t bl_version, uint32_t app_version);
void UpdateTelemetryNocTranslation(bool translation_enabled);
void UpdateTelemetryBoardPowerLimit(uint32_t power_limit);
void UpdateTelemetryTdpLimit(uint32_t tdp_limit);
void UpdateTelemetryThermTripCount(uint16_t therm_trip_count);
void UpdateTelemetryHostAiclkLimit(uint32_t fmax);
void UpdateTelemetryKernelThrottler(bool enabled, uint32_t stop_nops_freq);
bool GetTelemetryTagValid(uint16_t tag);
uint32_t GetTelemetryTag(uint16_t tag);

#endif
