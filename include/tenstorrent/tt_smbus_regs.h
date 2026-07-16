/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TT_SMBUS_MSGS_H_
#define TT_SMBUS_MSGS_H_

/**
 * @file
 * @brief SMBus command registers for CMFW <-> DMFW communication.
 *
 * This header defines the SMBus command codes used to communicate with the CMFW
 * over the SMBus interface. It is also used by the DMFW, as that firmware is the
 * SMBus master on PCIe cards. All SMBus command codes used by the CMFW should be
 * defined here.
 */

/**
 * @defgroup tt_smbus_regs SMBus Command Registers
 * @brief SMBus command set for DMFW (master) <-> CMFW (slave) communication.
 *
 * The DMFW acts as the SMBus master and issues these commands to the CMFW
 * (the SMBus slave). Each command has an access type and a fixed payload size:
 * - **RO** (read only): the master reads data from the CMFW.
 * - **WO** (write only): the master writes data to the CMFW.
 * - **RW** (read/write): a combined transaction where the master writes input
 *   bytes and reads response bytes in the same command.
 *
 * @note The "Bits In"/"Bits Out" sizes below (and the per-command byte layouts
 * in the enum) count **command payload only**. They do NOT include SMBus
 * transport framing added on the wire: block-transfer commands carry a leading
 * byte-count byte before the payload, and PEC-enabled commands carry a trailing
 * PEC byte. When a command is driven through the Zephyr SMBus controller API
 * (as production DMFW code does), the controller adds and strips these framing
 * bytes, so callers see only the payload. Tests that instead drive the bus with
 * the raw I2C controller API (`i2c_write_read()`) observe the full on-wire
 * layout — e.g. a CMFW_SMBUS_REQ read returns 1 count byte + 6 payload bytes
 * (+ a PEC byte if read). Size the receive buffer and parse offsets accordingly.
 *
 * @{
 */

/**
 * @brief SMBus command codes handled by the CMFW.
 *
 * | Command                                | Code | Access | Bits In | Bits Out | Description                                                        |
 * |----------------------------------------|------|--------|---------|----------|--------------------------------------------------------------------|
 * | CMFW_SMBUS_TELEMETRY_READ              | 0x02 | RW     | 8       | 56       | Get telemetry data                                                 |
 * | CMFW_SMBUS_TELEMETRY_WRITE             | 0x03 | RW     | 264     | 160      | Write telemetry data and relay control data                        |
 * | CMFW_SMBUS_UPDATE_ARC_STATE            | 0x04 | WO     | 24      | -        | Update the ARC state                                               |
 * | CMFW_SMBUS_REQ                         | 0x10 | RO     | -       | 48       | Read cm2dmMessage struct describing a request from the CMFW        |
 * | CMFW_SMBUS_ACK                         | 0x11 | WO     | 16      | -        | Ack a cm2dmMessage with sequence number and message ID             |
 * | CMFW_SMBUS_DM_STATIC_INFO              | 0x20 | WO     | 160     | -        | Write dmStaticInfo struct including DMFW version                   |
 * | CMFW_SMBUS_PING                        | 0x21 | WO     | 16      | -        | Write 0xA5A5 to respond to CMFW request `kCm2DmMsgIdPing`          |
 * | CMFW_SMBUS_FAN_SPEED                   | 0x22 | WO     | 16      | -        | Target fan speed percentage (0-100) broadcast to every CMFW        |
 * | CMFW_SMBUS_FAN_RPM                     | 0x23 | WO     | 16      | -        | Fan speed response to CMFW fan-speed-update requests               |
 * | CMFW_SMBUS_POWER_LIMIT                 | 0x24 | WO     | 16      | -        | Input power limit for the board                                    |
 * | CMFW_SMBUS_POWER_INSTANT              | 0x25 | WO     | 16      | -        | Current input power for the board                                  |
 * | CMFW_SMBUS_TELEMETRY_READ_CRC          | 0x26 | RW     | 8       | 56       | Select a telemetry tag for reading                                 |
 * | CMFW_SMBUS_TELEMETRY_READ_CRC_DATA     | 0x27 | RO     | -       | 56       | Read telemetry payload and response metadata after selecting a tag |
 * | CMFW_SMBUS_THERM_TRIP_COUNT            | 0x28 | WO     | 16      | -        | Thermal trip count                                                 |
 * | CMFW_SMBUS_DMC_LOG                     | 0x29 | WO     | <=256   | -        | Up to 32 bytes of data to log from the DMC side                    |
 * | CMFW_SMBUS_PING_V2                     | 0x2A | RO     | -       | 16       | Read data to verify the SMC got this ping request                  |
 * | CMFW_SMBUS_TEST_READ                   | 0xD8 | RO     | -       | 8        | Test read from CMFW scratch register                               |
 * | CMFW_SMBUS_TEST_WRITE                  | 0xD9 | WO     | 8       | -        | Test write to CMFW scratch register                                |
 * | CMFW_SMBUS_TEST_READ_WORD              | 0xDA | RO     | -       | 16       | Test read from CMFW scratch register                               |
 * | CMFW_SMBUS_TEST_WRITE_WORD             | 0xDB | WO     | 16      | -        | Test write to CMFW scratch register                                |
 * | CMFW_SMBUS_TEST_READ_BLOCK             | 0xDC | RO     | -       | 32       | Test read from CMFW scratch register                               |
 * | CMFW_SMBUS_TEST_WRITE_BLOCK            | 0xDD | WO     | 32      | -        | Test write to CMFW scratch register                                |
 * | CMFW_SMBUS_TEST_WRITE_BLOCK_READ_BLOCK | 0xDE | RW     | 32      | 32       | Test write to CMFW scratch register and read it back               |
 */
enum CMFWSMBusReg {
	/** @brief RW, 8 bits in, 56 bits out. Get telemetry data */
	CMFW_SMBUS_TELEMETRY_READ = 0x02,
	/** @brief RW, 264 bits in, 160 bits out. Write telem data and relay ctl data */
	CMFW_SMBUS_TELEMETRY_WRITE = 0x03,
	/** @brief WO, 24 bits. Update the Arc State */
	CMFW_SMBUS_UPDATE_ARC_STATE = 0x04,
	/** @brief RO, 48 bits. Read cm2dmMessage struct describing request from CMFW */
	CMFW_SMBUS_REQ = 0x10,
	/** @brief WO, 16 bits. Write with sequence number and message ID to ack cm2dmMessage */
	CMFW_SMBUS_ACK = 0x11,
	/** @brief WO, 160 bits. Write with dmStaticInfo struct including DMFW version */
	CMFW_SMBUS_DM_STATIC_INFO = 0x20,
	/** @brief WO, 16 bits. Write with 0xA5A5 to respond to CMFW request `kCm2DmMsgIdPing` */
	CMFW_SMBUS_PING = 0x21,
	/**
	 * @brief WO, 16 bits. Write with target fan speed percentage (0-100). Used by DMFW to
	 * broadcast forced fan speed to every CMFW so that each chip's telemetry reflects the
	 * board-level setting.
	 */
	CMFW_SMBUS_FAN_SPEED = 0x22,
	/**
	 * @brief WO, 16 bits. Write with fan speed to respond to CMFW request
	 * `kCm2DmMsgIdFanSpeedUpdate` or `kCm2DmMsgIdForcedFanSpeedUpdate`
	 */
	CMFW_SMBUS_FAN_RPM = 0x23,
	/** @brief WO, 16 bits. Write with input power limit for board */
	CMFW_SMBUS_POWER_LIMIT = 0x24,
	/** @brief WO, 16 bits. Write with current input power for board */
	CMFW_SMBUS_POWER_INSTANT = 0x25,
	/** @brief RW, 8 bits in, 56 bits out. Select a telemetry tag for reading. */
	CMFW_SMBUS_TELEMETRY_READ_CRC = 0x26,
	/** @brief RO, 56 bits out. Read the telemetry payload and response metadata after selecting
	 * a tag.
	 */
	CMFW_SMBUS_TELEMETRY_READ_CRC_DATA = 0x27,
	/** @brief WO, 16 bits. Write with therm trip count */
	CMFW_SMBUS_THERM_TRIP_COUNT = 0x28,
	/** @brief WO, Up to 32 bytes. Write with data to log from DMC side */
	CMFW_SMBUS_DMC_LOG = 0x29,

	/** @brief RO, 2 bytes. Read data to verify the SMC got this ping request */
	CMFW_SMBUS_PING_V2 = 0x2A,
	/** @brief RO, 8 bits. Issue a test read from CMFW scratch register */
	CMFW_SMBUS_TEST_READ = 0xD8,
	/** @brief WO, 8 bits. Write to CMFW scratch register */
	CMFW_SMBUS_TEST_WRITE = 0xD9,
	/** @brief RO, 16 bits. Issue a test read from CMFW scratch register */
	CMFW_SMBUS_TEST_READ_WORD = 0xDA,
	/** @brief WO, 16 bits. Write to CMFW scratch register */
	CMFW_SMBUS_TEST_WRITE_WORD = 0xDB,
	/** @brief RO, 32 bits. Issue a test read from CMFW scratch register */
	CMFW_SMBUS_TEST_READ_BLOCK = 0xDC,
	/** @brief WO, 32 bits. Write to CMFW scratch register */
	CMFW_SMBUS_TEST_WRITE_BLOCK = 0xDD,
	/** @brief WR, 32 bits I/O. Write to CMFW scratch register and read it back. */
	CMFW_SMBUS_TEST_WRITE_BLOCK_READ_BLOCK = 0xDE,
	/** @brief Sentinel marking the number of SMBus command codes; not a real command. */
	CMFW_SMBUS_MSG_MAX,
};

/** @} */ /* end of tt_smbus_regs group */

#endif /* TT_SMBUS_MSGS_H_ */
