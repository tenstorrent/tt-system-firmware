/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

/* Scratch registers used for status and error reporting */
#ifndef STATUS_REG_H
#define STATUS_REG_H

#include <stdint.h>

#define RESET_UNIT_SCRATCH_RAM_BASE_ADDR 0x80030400
#define RESET_UNIT_SCRATCH_RAM_REG_ADDR(n)                                                         \
	(RESET_UNIT_SCRATCH_RAM_BASE_ADDR + sizeof(uint32_t) * (n))

#define RESET_UNIT_SCRATCH_BASE_ADDR   0x80030060
#define RESET_UNIT_SCRATCH_REG_ADDR(n) (RESET_UNIT_SCRATCH_BASE_ADDR + sizeof(uint32_t) * (n))

/* SCRATCH_[0-7] */
#define STATUS_POST_CODE_REG_ADDR      RESET_UNIT_SCRATCH_REG_ADDR(0)
/* Cable power limit written by DMC via JTAG before ARC boot.
 * Format: [31:16] = magic marker, [15:0] = power limit in watts
 * Magic marker presence indicates DMC supports this feature.
 * If magic marker absent (legacy DMC), SMC skips cable fault detection.
 * If magic marker present and power_limit=0, cable fault is detected.
 */
#define DMC_CABLE_POWER_LIMIT_REG_ADDR RESET_UNIT_SCRATCH_REG_ADDR(1)
#define CABLE_POWER_LIMIT_MAGIC        0xCAB10000 /* Magic marker in upper 16 bits */
#define CABLE_POWER_LIMIT_MAGIC_MASK   0xFFFF0000
#define CABLE_POWER_LIMIT_VALUE_MASK   0x0000FFFF

/* SCRATCH_RAM[0-63] */
#define STATUS_FW_VERSION_REG_ADDR           RESET_UNIT_SCRATCH_RAM_REG_ADDR(0)
/* SCRATCH_RAM_1 is reserved for the security handshake used by bootcode */
#define STATUS_BOOT_STATUS0_REG_ADDR         RESET_UNIT_SCRATCH_RAM_REG_ADDR(2)
#define STATUS_BOOT_STATUS1_REG_ADDR         RESET_UNIT_SCRATCH_RAM_REG_ADDR(3)
#define STATUS_ERROR_STATUS0_REG_ADDR        RESET_UNIT_SCRATCH_RAM_REG_ADDR(4)
#define STATUS_ERROR_STATUS1_REG_ADDR        RESET_UNIT_SCRATCH_RAM_REG_ADDR(5)
#define STATUS_INTERFACE_TABLE_BASE_REG_ADDR RESET_UNIT_SCRATCH_RAM_REG_ADDR(6)
/* SCRATCH_RAM_7 is reserved for possible future interface table uses */
#define STATUS_MSG_Q_STATUS_REG_ADDR         RESET_UNIT_SCRATCH_RAM_REG_ADDR(8)
#define STATUS_MSG_Q_ERR_FLAGS_REG_ADDR      RESET_UNIT_SCRATCH_RAM_REG_ADDR(9)
#define SPI_BUFFER_INFO_REG_ADDR             RESET_UNIT_SCRATCH_RAM_REG_ADDR(10)
#define STATUS_MSG_Q_INFO_REG_ADDR           RESET_UNIT_SCRATCH_RAM_REG_ADDR(11)
/**
 * @ingroup telemetry
 * @brief Register address pointing to the telemetry data buffer.
 *
 * This register holds the address of the telemetry data buffer, which contains
 * dynamically updated telemetry values.
 */
#define TELEMETRY_DATA_REG_ADDR              RESET_UNIT_SCRATCH_RAM_REG_ADDR(12)
/**
 * @ingroup telemetry
 * @brief Register address pointing to the telemetry table.
 *
 * This register holds the address of the global telemetry table, which contains
 * metadata and telemetry data.
 */
#define TELEMETRY_TABLE_REG_ADDR             RESET_UNIT_SCRATCH_RAM_REG_ADDR(13)
#define PCIE_INIT_CPL_TIME_REG_ADDR          RESET_UNIT_SCRATCH_RAM_REG_ADDR(14)
#define CMFW_START_TIME_REG_ADDR             RESET_UNIT_SCRATCH_RAM_REG_ADDR(15)
#define ARC_START_TIME_REG_ADDR              RESET_UNIT_SCRATCH_RAM_REG_ADDR(16)
#define PERST_TO_DMFW_INIT_DONE_REG_ADDR     RESET_UNIT_SCRATCH_RAM_REG_ADDR(17)
#define PING_DMFW_DURATION_REG_ADDR          RESET_UNIT_SCRATCH_RAM_REG_ADDR(18)
#define I2C0_TARGET_DEBUG_STATE_REG_ADDR     RESET_UNIT_SCRATCH_RAM_REG_ADDR(19)
#define I2C0_TARGET_DEBUG_STATE_2_REG_ADDR   RESET_UNIT_SCRATCH_RAM_REG_ADDR(20)
#define ARC_HANG_PC                          RESET_UNIT_SCRATCH_RAM_REG_ADDR(21)
/**
 * @brief Register address of the Metal runtime telemetry buffer.
 *
 * Holds the CSM address of tt_runtime_telemetry. Written once during init
 * and not updated until the next chip reset. Fixed at link time for a
 * given CMFW build.
 */
#define RUNTIME_TELEMETRY_ADDR_REG_ADDR      RESET_UNIT_SCRATCH_RAM_REG_ADDR(22)
/**
 * @brief Register holding the size in bytes of the Metal runtime telemetry buffer.
 *
 * Written once during init and not updated until the next chip reset.
 * Fixed at compile time for a given CMFW build.
 */
#define RUNTIME_TELEMETRY_SIZE_REG_ADDR      RESET_UNIT_SCRATCH_RAM_REG_ADDR(23)

#define STATUS_FW_VUART_REG_ADDR(n) RESET_UNIT_SCRATCH_RAM_REG_ADDR(40 + (n))
/* SCRATCH_RAM_40 - SCRATCH_RAM_41 reserved for virtual uarts */
#define STATUS_FW_SCRATCH_REG_ADDR  RESET_UNIT_SCRATCH_RAM_REG_ADDR(63)

typedef struct {
	uint32_t msg_queue_ready: 1;
	uint32_t hw_init_status: 2;
	uint32_t fw_id: 4;
	uint32_t spare: 25;
} STATUS_BOOT_STATUS0_reg_t;

typedef union {
	uint32_t val;
	STATUS_BOOT_STATUS0_reg_t f;
} STATUS_BOOT_STATUS0_reg_u;

#endif
