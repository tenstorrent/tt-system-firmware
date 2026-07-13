/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TT_SMBUS_MSGS_H_
#define TT_SMBUS_MSGS_H_

/*
 * This header file contains the definitions for the SMBus registers used to
 * communicate with the CMFW over the SMBus interface. It is also used by
 * the DMFW, as that FW is the SMBus master on PCIe cards.
 * All SMBus registers used by the CMFW should be defined here.
 */

enum CMFWSMBusReg {
	/* RW, 8 bits in, 56 bits out. Get telemetry data */
	CMFW_SMBUS_TELEMETRY_READ = 0x02,
	/* RW, 264 bits in, 160 bits out. Write telem data and relay ctl data */
	CMFW_SMBUS_TELEMETRY_WRITE = 0x03,
	/* W0, 24 bits. Update the Arc State */
	CMFW_SMBUS_UPDATE_ARC_STATE = 0x04,
	/* RO, 48 bits. Read cm2dmMessage struct describing request from CMFW */
	CMFW_SMBUS_REQ = 0x10,
	/* WO, 16 bits. Write with sequence number and message ID to ack cm2dmMessage */
	CMFW_SMBUS_ACK = 0x11,
	/* WO, 160 bits. Write with dmStaticInfo struct including DMFW version */
	CMFW_SMBUS_DM_STATIC_INFO = 0x20,
	/* WO, 16 bits. Write with 0xA5A5 to respond to CMFW request `kCm2DmMsgIdPing` */
	CMFW_SMBUS_PING = 0x21,
	/* WO, 16 bits. Write with target fan speed percentage (0-100). Used by DMFW to broadcast
	 * forced fan speed to every CMFW so that each chip's telemetry reflects the board-level
	 * setting.
	 */
	CMFW_SMBUS_FAN_SPEED = 0x22,
	/* WO, 16 bits. Write with fan speed to responsd to CMFW request
	 * `kCm2DmMsgIdFanSpeedUpdate` or `kCm2DmMsgIdForcedFanSpeedUpdate`
	 */
	CMFW_SMBUS_FAN_RPM = 0x23,
	/* WO, 16 bits. Write with input power limit for board */
	CMFW_SMBUS_POWER_LIMIT = 0x24,
	/* WO, 16 bits. Write with current input power for board */
	CMFW_SMBUS_POWER_INSTANT = 0x25,
	/* RW, 8 bits in, 56 bits out. Select a telemetry tag for reading.
	 */
	CMFW_SMBUS_TELEMETRY_READ_CRC = 0x26,
	/* RO, 56 bits out. Read the telemetry payload and response metadata after selecting a tag.
	 */
	CMFW_SMBUS_TELEMETRY_READ_CRC_DATA = 0x27,
	/* WO, 16 bits. Write with therm trip count */
	CMFW_SMBUS_THERM_TRIP_COUNT = 0x28,
	/* WO, Up to 32 bytes. Write with data to log from DMC side */
	CMFW_SMBUS_DMC_LOG = 0x29,

	/* RO, 2 bytes. Read data to verify the SMC got this ping request */
	CMFW_SMBUS_PING_V2 = 0x2A,
	/* RO, 8 bits. Issue a test read from CMFW scratch register */
	CMFW_SMBUS_TEST_READ = 0xD8,
	/* WO, 8 bits. Write to CMFW scratch register */
	CMFW_SMBUS_TEST_WRITE = 0xD9,
	/* RO, 16 bits. Issue a test read from CMFW scratch register */
	CMFW_SMBUS_TEST_READ_WORD = 0xDA,
	/* WO, 16 bits. Write to CMFW scratch register */
	CMFW_SMBUS_TEST_WRITE_WORD = 0xDB,
	/* RO, 32 bits. Issue a test read from CMFW scratch register */
	CMFW_SMBUS_TEST_READ_BLOCK = 0xDC,
	/* WO, 32 bits. Write to CMFW scratch register */
	CMFW_SMBUS_TEST_WRITE_BLOCK = 0xDD,
	/* WR, 32 bits I/O. Write to CMFW scratch register and read it back. */
	CMFW_SMBUS_TEST_WRITE_BLOCK_READ_BLOCK = 0xDE,
	CMFW_SMBUS_MSG_MAX,
};

/* Request IDs that the CMFW can issue within the */

#endif /* TT_SMBUS_MSGS_H_ */
