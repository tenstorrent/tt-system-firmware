/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _GDDR_H_
#define _GDDR_H_

#include "gddr_telemetry_table.h"
#include "gddr_params.h"

#include <zephyr/sys_clock.h>

#define MIN_GDDR_SPEED             12000
#define MAX_GDDR_SPEED             20000
#define GDDR_SPEED_TO_MEMCLK_RATIO 16
#define NUM_GDDR                   8
#define NUM_MRISC_NOC2AXI_PORT     3

/* MRISC FW telemetry base addr */
#define GDDR_TELEMETRY_TABLE_ADDR 0x8000
#define GDDR_MSG_STRUCT_ADDR      0x6000

#define RISC_CTRL_A_SCRATCH_0__REG_ADDR 0xFFB14010
#define RISC_CTRL_A_SCRATCH_1__REG_ADDR 0xFFB14014
#define RISC_CTRL_A_SCRATCH_2__REG_ADDR 0xFFB14018
#define MRISC_INIT_STATUS               RISC_CTRL_A_SCRATCH_0__REG_ADDR
#define MRISC_POST_CODE                 RISC_CTRL_A_SCRATCH_1__REG_ADDR
#define MRISC_MSG_REGISTER              RISC_CTRL_A_SCRATCH_2__REG_ADDR

#define MRISC_INIT_FINISHED            0xdeadbeef
#define MRISC_INIT_FAILED              0xfa11
#define MRISC_INIT_BEFORE              0x11111111
#define MRISC_INIT_STARTED             0x0
#define MRISC_INIT_TIMEOUT             1000 /* In ms */
#define MRISC_MEMTEST_TIMEOUT          1000 /* In ms */
#define MRISC_POWER_SETTING_TIMEOUT_MS 1000

/* Defined by MRISC FW */

/** @brief MRISC message type when no active message is signalled;
 *  acts as a completion signal from MRISC
 */
#define MRISC_MSG_TYPE_NONE          0
/** @brief MRISC message to set the phy to power down state.*/
#define MRISC_MSG_TYPE_PHY_POWERDOWN 1
/** @brief MRISC message to set the phy to wake up state.*/
#define MRISC_MSG_TYPE_PHY_WAKEUP    2
/** @brief MRISC message to run the memory test.*/
#define MRISC_MSG_TYPE_RUN_MEMTEST   8

int read_gddr_telemetry_table(uint8_t gddr_inst, gddr_telemetry_table_t *gddr_telemetry);

/** @brief Top and bottom DRAM die temperatures for a single GDDR instance (degrees Celsius). */
struct gddr_inst_temp {
	uint8_t top;
	uint8_t bottom;
};

/** @brief Per-instance GDDR die temperatures plus the max across all dies. */
struct gddr_temps {
	uint32_t max_temp;                    /* degC, max across all read dies */
	struct gddr_inst_temp inst[NUM_GDDR]; /* per-instance top/bottom die temps */
};

/**
 * @brief Read the DRAM die temperatures for all enabled GDDR instances.
 *
 * Disabled instances are left zeroed. Reads that fail are logged and skipped, leaving the
 * corresponding entry zeroed while the remaining instances are still read.
 *
 * @param [out] temps Destination struct. Fully overwritten on entry.
 * @return 0 if all enabled instances were read successfully. If one or more reads failed,
 *         the last error code encountered is returned (the rest are still read).
 */
int get_gddr_temps(struct gddr_temps *temps);

/** @brief BIST status bitmasks (one bit per GDDR instance). */
struct gddr_bist_info {
	uint8_t complete;
	uint8_t failed;
};

/**
 * @brief Get GDDR BIST status
 *
 * @return Per-instance bitmasks indicating which instances have completed BIST
 *         and which have failed.
 */
struct gddr_bist_info get_gddr_bist_info(void);

/** @brief Sets the MRISC power setting for all active MRISCs
 * @param [in] on `true` to send MRISCs the @ref MRISC_MSG_TYPE_PHY_WAKEUP command <br>
 * `false` to send MRISCs the @ref MRISC_MSG_TYPE_PHY_POWERDOWN command
 * @return 0 on success. Negative error code on failure.
 */
int32_t set_mrisc_power_setting(bool on);

#endif
