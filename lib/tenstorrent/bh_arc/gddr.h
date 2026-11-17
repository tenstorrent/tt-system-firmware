/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _GDDR_H_
#define _GDDR_H_

#include "gddr_telemetry_table.h"
#include "gddr_params.h"

#include <zephyr/sys/util.h>
#include <zephyr/sys_clock.h>

/* Oldest telemetry table version the SMC can still consume. Tables older than
 * GDDR_TELEMETRY_TABLE_T_VERSION simply lack the newer trailing fields.
 */
#define GDDR_TELEMETRY_TABLE_T_VERSION_MIN 2

/* gddr_params_table_t feature_bits fields (bh-mrisc feature_bit_t) */
#define GDDR_FEATURE_AC_TIMING_OVERRIDE       BIT(0)
#define GDDR_FEATURE_CTRL_PHY_PARAMS_OVERRIDE BIT(1)
#define GDDR_FEATURE_ENABLE_INLINE_ECC        BIT(2)
#define GDDR_FEATURE_CA_SETTINGS_VALID        BIT(3)
/* MRISC 2.17: VREFC margin check / 1D sweep. 2.18+: five-point check / 2D eye centering. */
#define GDDR_FEATURE_ENABLE_CA_MARGIN_CHECK   BIT(4)
#define GDDR_FEATURE_ENABLE_CA_SWEEP_FALLBACK BIT(5)
/* MRISC 2.18+ (params table v9) */
#define GDDR_FEATURE_CA_PHASE_OFFSETS_VALID   BIT(6)
#define GDDR_FEATURE_CA_CENTER_PER_CH_VREFC   BIT(7)
/* MRISC 2.18: 1 = halt on a failed first training instead of running the CA boot rescue. The
 * SMC leaves it clear, so the rescue runs (with GDDR_FEATURE_ENABLE_CA_SWEEP_FALLBACK).
 */
#define GDDR_FEATURE_DISABLE_CA_BOOT_RESCUE   BIT(8)

/* Telemetry v4 ca_center_flags: the first training failed and the CA boot rescue found the
 * point CA centering started from (ca_rescue_* hold why and how long it took).
 */
#define GDDR_CA_FLAG_RESCUED BIT(4)

/* Telemetry v4 ca_center_status: boot CA verdict (bh-mrisc ca_verdict_e). 7 and 8 halt the
 * MRISC with GDDR_ERROR_CA_SWEEP_FAIL; only 1 and 2 are latched to gddrcal.
 */
#define GDDR_CA_VERDICT_NOT_RUN             0 /* CA check and centering both off */
#define GDDR_CA_VERDICT_PASS_SEEDED         1 /* passed on the seeded term / drive pair */
#define GDDR_CA_VERDICT_PASS_NEW_PAIR       2 /* the term / drive search found a passing pair */
#define GDDR_CA_VERDICT_CHECK_FAIL          3 /* check only: failed, plain training kept */
#define GDDR_CA_VERDICT_DEGRADED_TIME_CAP   4 /* time cap before a pair passed, booted on best */
#define GDDR_CA_VERDICT_DEGRADED_NO_PASS    5 /* no pair passed, booted on the best */
#define GDDR_CA_VERDICT_DEGRADED_SCAN_FAIL  6 /* scan incomplete / retrained, seeded point */
#define GDDR_CA_VERDICT_FATAL_NO_EYE        7
#define GDDR_CA_VERDICT_FATAL_RECOVERY_FAIL 8

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

#define MRISC_INIT_FINISHED 0xdeadbeef
#define MRISC_INIT_FAILED   0xfa11
#define MRISC_INIT_BEFORE   0x11111111
#define MRISC_INIT_STARTED  0x0
#ifdef CONFIG_TT_BH_ARC_GDDR_CA_LATCH
/* MRISC CA tuning at boot: the 2.17 1D sweep fallback measures ~28 s worst case on a P300.
 * 2.18 caps its CA boot rescue (only after a failed first training) at ca_rescue_time_cap_ms
 * (15 s) and the 2D eye centering with its CA termination x PHY CA TX drive search at
 * ca_center_time_cap_ms (20 s). The controllers run in parallel and healthy boots finish in
 * a few seconds - this only delays the failure verdict for controllers already in trouble.
 */
#define MRISC_INIT_TIMEOUT 40000 /* In ms */
#else
#define MRISC_INIT_TIMEOUT 1000 /* In ms */
#endif
/* MRISC's MT_DONE wait is 1 s; allow for the message round trip. */
#define MRISC_MEMTEST_TIMEOUT          1500 /* In ms */
/* A healthy wake / powerdown takes milliseconds. Covers an SR entry or PHY low-power ack that
 * times out on MRISC (2 s) and is backed out with an SR exit.
 */
#define MRISC_POWER_SETTING_TIMEOUT_MS 6000

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

/** @brief GDDR instances that are both unharvested and enabled by the FW table dram_mask. */
uint32_t GetDramMask(void);

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

/**
 * @brief Get the noc2axi port MRISC FW is loaded on for a GDDR instance.
 *
 * @param [in] gddr_inst GDDR controller instance (0 - @ref NUM_GDDR - 1).
 * @return noc2axi port index (0, 1 or 2).
 */
uint8_t get_gddr_mrisc_noc2axi_port(uint8_t gddr_inst);

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
