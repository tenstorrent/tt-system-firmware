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
 * <table>
 * <tr>
 * <th>Command</th><th>Code</th><th>Access</th><th>Description</th>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_TELEMETRY_READ</td><td>0x02</td><td>RW</td>
 * <td>Get telemetry data</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_TELEMETRY_WRITE</td><td>0x03</td><td>RW</td>
 * <td>Write telemetry data and relay control data</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_UPDATE_ARC_STATE</td><td>0x04</td><td>WO</td>
 * <td>Update the ARC state</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_REQ</td><td>0x10</td><td>RO</td>
 * <td>Read cm2dmMessage struct describing a request from the CMFW</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_ACK</td><td>0x11</td><td>WO</td>
 * <td>Ack a cm2dmMessage with sequence number and message ID</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_DM_STATIC_INFO</td><td>0x20</td><td>WO</td>
 * <td>Write dmStaticInfo struct including DMFW version</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_PING</td><td>0x21</td><td>WO</td>
 * <td>Write 0xA5A5 to respond to CMFW request `kCm2DmMsgIdPing`</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_FAN_SPEED</td><td>0x22</td><td>WO</td>
 * <td>Target fan speed percentage (0-100) broadcast to every CMFW</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_FAN_RPM</td><td>0x23</td><td>WO</td>
 * <td>Fan speed response to CMFW fan-speed-update requests</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_POWER_LIMIT</td><td>0x24</td><td>WO</td>
 * <td>Input power limit for the board</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_POWER_INSTANT</td><td>0x25</td><td>WO</td>
 * <td>Current input power for the board</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_TELEMETRY_READ_CRC</td><td>0x26</td><td>WO</td>
 * <td>Select a telemetry tag for reading</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_TELEMETRY_READ_CRC_DATA</td><td>0x27</td><td>RO</td>
 * <td>Read telemetry payload and response metadata after selecting a tag</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_THERM_TRIP_COUNT</td><td>0x28</td><td>WO</td>
 * <td>Thermal trip count</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_DMC_LOG</td><td>0x29</td><td>WO</td>
 * <td>Up to 32 bytes of data to log from the DMC side</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_PING_V2</td><td>0x2A</td><td>RO</td>
 * <td>Read data to verify the SMC got this ping request</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_TEST_READ</td><td>0xD8</td><td>RO</td>
 * <td>Test read from CMFW scratch register</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_TEST_WRITE</td><td>0xD9</td><td>WO</td>
 * <td>Test write to CMFW scratch register</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_TEST_READ_WORD</td><td>0xDA</td><td>RO</td>
 * <td>Test read from CMFW scratch register</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_TEST_WRITE_WORD</td><td>0xDB</td><td>WO</td>
 * <td>Test write to CMFW scratch register</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_TEST_READ_BLOCK</td><td>0xDC</td><td>RO</td>
 * <td>Test read from CMFW scratch register</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_TEST_WRITE_BLOCK</td><td>0xDD</td><td>WO</td>
 * <td>Test write to CMFW scratch register</td>
 * </tr>
 * <tr>
 * <td>@ref CMFW_SMBUS_TEST_WRITE_BLOCK_READ_BLOCK</td><td>0xDE</td><td>RW</td>
 * <td>Test write to CMFW scratch register and read it back</td>
 * </tr>
 * </table>
 */
enum CMFWSMBusReg {
	/**
	 * @brief Get telemetry data.
	 *
	 * Input (1 byte):
	 * - byte 0: telemetry tag (register index) selecting which value to read.
	 *
	 * Output (7 bytes, little-endian):
	 * - byte 0:    tag valid flag (0 = valid, 1 = tag out of range).
	 * - bytes 1-2: reserved (0).
	 * - bytes 3-6: 32-bit telemetry value for the tag (0xFFFFFFFF if invalid).
	 */
	CMFW_SMBUS_TELEMETRY_READ = 0x02,
	/**
	 * @brief Write telemetry data and relay control data.
	 *
	 * Input (33 bytes): telemetry payload from the DMC. The CMFW currently
	 * validates only the length and does not interpret the contents.
	 *
	 * Output (20 bytes, little-endian):
	 * - bytes 0-10:  reserved (0).
	 * - bytes 11-14: control-data bitfield (LSB first):
	 *   - bit 0-7:   pcie_index.
	 *   - bit 8:     trigger_asic_reset.
	 *   - bit 9:     trigger_spi_copy_1_to_r.
	 *   - bit 10:    arc_state_a3_req.
	 *   - bit 11:    arc_state_a0_req.
	 *   - bit 12:    trigger_asic_and_m3_reset.
	 *   - bit 13:    clear_num_auto_reset.
	 *   - bit 14-31: spare.
	 * - bytes 15-18: reserved (0).
	 * - byte 19:     CRC-8 PEC (poly 0x7) over the size byte and bytes 0-18.
	 *                Not SMBus-compliant; kept for WH compatibility.
	 */
	CMFW_SMBUS_TELEMETRY_WRITE = 0x03,
	/**
	 * @brief Update the ARC state.
	 *
	 * Input (3 bytes):
	 * - byte 0: ASIC state value.
	 * - byte 1: signature, must be 0xDE.
	 * - byte 2: signature, must be 0xAF.
	 */
	CMFW_SMBUS_UPDATE_ARC_STATE = 0x04,
	/**
	 * @brief Read cm2dmMessage struct describing a request from the CMFW.
	 *
	 * Output (6 bytes, little-endian), a @ref cm2dmMessage struct:
	 * - byte 0:    msg_id (a Cm2DmMsgId, e.g. reset / ping / fan-speed request).
	 * - byte 1:    seq_num (incremented per issued message).
	 * - bytes 2-5: data (message-specific 32-bit payload).
	 *
	 * All bytes are zero when no message is pending.
	 */
	CMFW_SMBUS_REQ = 0x10,
	/**
	 * @brief Ack a cm2dmMessage with sequence number and message ID.
	 *
	 * Input (2 bytes), a @ref cm2dmAck struct:
	 * - byte 0: msg_id  (must match the pending message's msg_id).
	 * - byte 1: seq_num (must match the pending message's seq_num).
	 *
	 * On a match the pending cm2dmMessage is cleared.
	 */
	CMFW_SMBUS_ACK = 0x11,
	/**
	 * @brief Write dmStaticInfo struct including DMFW version.
	 *
	 * Input (24 bytes, little-endian), a @ref dmStaticInfo of six uint32 fields:
	 * - bytes 0-3:   version (must be non-zero, else the write is rejected).
	 * - bytes 4-7:   bl_version (bootloader version).
	 * - bytes 8-11:  app_version (application version).
	 * - bytes 12-15: arc_start_time (timestamp in ASIC refclk @ 50 MHz).
	 * - bytes 16-19: dm_init_duration (duration in DMC refclk @ 64 MHz).
	 * - bytes 20-23: arc_hang_pc (PC of last ARC hang; recorded only if non-zero).
	 */
	CMFW_SMBUS_DM_STATIC_INFO = 0x20,
	/**
	 * @brief Write 0xA5A5 to respond to CMFW request `kCm2DmMsgIdPing`.
	 *
	 * Input (2 bytes): 16-bit value, must equal 0xA5A5 (little-endian). Responds to
	 * the CMFW request `kCm2DmMsgIdPing`.
	 */
	CMFW_SMBUS_PING = 0x21,
	/**
	 * @brief Target fan speed percentage (0-100) broadcast to every CMFW.
	 *
	 * Input (2 bytes): 16-bit target fan speed percentage (0-100), little-endian.
	 * Broadcast by the DMFW to every CMFW so each chip's telemetry reflects the
	 * board-level setting.
	 */
	CMFW_SMBUS_FAN_SPEED = 0x22,
	/**
	 * @brief Fan speed response to CMFW fan-speed-update requests.
	 *
	 * Input (2 bytes): 16-bit fan speed in RPM, little-endian. Responds to the CMFW
	 * request `kCm2DmMsgIdFanSpeedUpdate` or `kCm2DmMsgIdForcedFanSpeedUpdate`.
	 */
	CMFW_SMBUS_FAN_RPM = 0x23,
	/**
	 * @brief Input power limit for the board.
	 *
	 * Input (2 bytes): 16-bit board input power limit (little-endian). Clamped to
	 * the board limit and applied to the board-power and Doppler-slow throttlers.
	 */
	CMFW_SMBUS_POWER_LIMIT = 0x24,
	/**
	 * @brief Current input power for the board.
	 *
	 * Input (2 bytes): 16-bit current input power (little-endian). Stored with an
	 * additional board-power offset added and triggers a DVFS timer adjustment.
	 */
	CMFW_SMBUS_POWER_INSTANT = 0x25,
	/**
	 * @brief Select a telemetry tag for reading.
	 *
	 * Input (1 byte):
	 * - byte 0: telemetry tag to select for the subsequent
	 *   @ref CMFW_SMBUS_TELEMETRY_READ_CRC_DATA read.
	 *
	 * This command itself returns no data; the value is read back with the separate
	 * @ref CMFW_SMBUS_TELEMETRY_READ_CRC_DATA (0x27) command. "CRC" refers to the
	 * transport PEC byte appended by the SMBus layer, not a CRC computed by the
	 * handler.
	 */
	CMFW_SMBUS_TELEMETRY_READ_CRC = 0x26,
	/**
	 * @brief Read telemetry payload and response metadata after selecting a tag.
	 *
	 * Output (7 bytes, little-endian), same layout as @ref CMFW_SMBUS_TELEMETRY_READ:
	 * - byte 0:    tag valid flag (0 = valid, 1 = tag out of range).
	 * - bytes 1-2: reserved (0).
	 * - bytes 3-6: 32-bit telemetry value for the selected tag.
	 */
	CMFW_SMBUS_TELEMETRY_READ_CRC_DATA = 0x27,
	/**
	 * @brief Thermal trip count.
	 *
	 * Input (2 bytes): 16-bit thermal trip count, little-endian.
	 */
	CMFW_SMBUS_THERM_TRIP_COUNT = 0x28,
	/**
	 * @brief Up to 32 bytes of data to log from the DMC side.
	 *
	 * Input (variable, up to 32 bytes): raw log characters written to the DMC
	 * vUART; no structured fields.
	 */
	CMFW_SMBUS_DMC_LOG = 0x29,

	/**
	 * @brief Read data to verify the SMC got this ping request.
	 *
	 * Output (2 bytes): fixed sentinel 0xA5, 0xA5. Read to verify the SMC received
	 * this ping request.
	 */
	CMFW_SMBUS_PING_V2 = 0x2A,
	/**
	 * @brief Test read from CMFW scratch register.
	 *
	 * Output (1 byte): low byte of the CMFW scratch register.
	 */
	CMFW_SMBUS_TEST_READ = 0xD8,
	/**
	 * @brief Test write to CMFW scratch register.
	 *
	 * Input (1 byte): value written to the CMFW scratch register.
	 */
	CMFW_SMBUS_TEST_WRITE = 0xD9,
	/**
	 * @brief Test read from CMFW scratch register.
	 *
	 * Output (2 bytes): low 16 bits of the scratch register, little-endian.
	 */
	CMFW_SMBUS_TEST_READ_WORD = 0xDA,
	/**
	 * @brief Test write to CMFW scratch register.
	 *
	 * Input (2 bytes): value written to the scratch register, little-endian.
	 */
	CMFW_SMBUS_TEST_WRITE_WORD = 0xDB,
	/**
	 * @brief Test read from CMFW scratch register.
	 *
	 * Output (4 bytes): full 32-bit scratch register, little-endian.
	 */
	CMFW_SMBUS_TEST_READ_BLOCK = 0xDC,
	/**
	 * @brief Test write to CMFW scratch register.
	 *
	 * Input (4 bytes): value written to the scratch register, little-endian.
	 */
	CMFW_SMBUS_TEST_WRITE_BLOCK = 0xDD,
	/**
	 * @brief Test write to CMFW scratch register and read it back.
	 *
	 * Input (4 bytes): value written to the scratch register, little-endian.
	 * Output (4 bytes): the value read back, little-endian.
	 */
	CMFW_SMBUS_TEST_WRITE_BLOCK_READ_BLOCK = 0xDE,
	/**
	 * @brief One past the last defined command code; not a real command.
	 *
	 * The command codes above are sparse, so this is not a count of commands. It is
	 * used as a known-invalid command code (e.g. in the smbus_target tests).
	 */
	CMFW_SMBUS_MSG_MAX,
};

/** @} */ /* end of tt_smbus_regs group */

#endif /* TT_SMBUS_MSGS_H_ */
