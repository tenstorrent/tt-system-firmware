/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_TT_GRENDEL_I3C_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_TT_GRENDEL_I3C_H_

/**
 * @file
 * @brief Inter-die command protocol over I3C
 *
 * Lets the die that owns a bring-up sequence (the I3C controller, Keraunos)
 * run work on another die (the I3C target, Mimir). The first user is D2D link
 * bring-up, where both ends have to be loaded before either is started or the
 * link does not train, and with sideband synchronisation disabled nothing in
 * hardware arranges that.
 *
 * Commands are plain I3C private writes of an opcode byte followed by an
 * opcode-specific payload struct (below), sized to fit within
 * TT_GRENDEL_I3C_CMD_PAYLOAD_MAX_LEN. The target runs the command on a
 * workqueue and then writes a @ref tt_grendel_i3c_response back, which the
 * controller collects with a private read. There is no IBI, so the controller
 * retries that read until the response echoes the opcode it sent.
 *
 * Adding a command needs a new opcode, a payload struct if it carries one, a
 * handler on the target, and a wrapper function on the controller side.
 */

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/toolchain.h>

/** @brief Ping the target to check it is up. Carries no payload. */
#define TT_GRENDEL_I3C_CMD_PING 0x01U

/**
 * @brief Reset-release one of the target's D2D tiles and load its firmware.
 *
 * Leaves the tile's Rocket in reset, so the link does not begin training
 * until TT_GRENDEL_I3C_CMD_D2D_START. The firmware image is the one already
 * staged in the target's local memory; it is not carried over I3C. The tile
 * is selected by tt_grendel_i3c_d2d_req::instance.
 */
#define TT_GRENDEL_I3C_CMD_D2D_INIT 0x02U

/**
 * @brief Release the D2D Rocket selected by tt_grendel_i3c_d2d_req::instance so
 * its loaded firmware runs.
 */
#define TT_GRENDEL_I3C_CMD_D2D_START 0x03U

/** @brief Result reported in @ref tt_grendel_i3c_response. */
enum tt_grendel_i3c_result {
	/** The command completed successfully. */
	TT_GRENDEL_I3C_OK = 0,
	/** The command failed on the target. */
	TT_GRENDEL_I3C_NOT_OK = 1,
};

/** @brief Payload for TT_GRENDEL_I3C_CMD_D2D_INIT and TT_GRENDEL_I3C_CMD_D2D_START. */
struct tt_grendel_i3c_d2d_req {
	/** D2D tile index the opcode applies to. */
	uint8_t instance;
} __packed;

/** @brief Largest payload carried by any TT_GRENDEL_I3C_CMD_* opcode. */
#define TT_GRENDEL_I3C_CMD_PAYLOAD_MAX_LEN sizeof(struct tt_grendel_i3c_d2d_req)

/**
 * @brief Command frame, written by the controller.
 *
 * The payload is zero-padded to TT_GRENDEL_I3C_CMD_PAYLOAD_MAX_LEN so every
 * command is the same size on the wire; opcodes interpret only the leading
 * bytes their payload struct defines.
 */
struct tt_grendel_i3c_cmd_frame {
	/** One of the TT_GRENDEL_I3C_CMD_* opcodes. */
	uint8_t opcode;
	uint8_t payload[TT_GRENDEL_I3C_CMD_PAYLOAD_MAX_LEN];
} __packed;

/** @brief Response frame, written by the target once a command has run. */
struct tt_grendel_i3c_response {
	/** One of @ref tt_grendel_i3c_result. */
	uint8_t result;
	/** Echoes the opcode being answered, so the controller can match it. */
	uint8_t opcode;
	uint8_t reserved[2];
} __packed;

BUILD_ASSERT(sizeof(struct tt_grendel_i3c_response) == 4, "response frame must stay 4 bytes");

/**
 * @brief Wait until the remote die is on the bus and answering.
 *
 * The target only registers once the remote die has booted far enough to run
 * its driver, so pings are expected to fail for a while. Retry until one is
 * answered.
 *
 * @param dev Grendel I3C controller device
 * @param timeout How long to wait for the target to appear
 *
 * @retval 0 once the target answers
 * @retval -ETIMEDOUT if it never did
 */
int tt_grendel_i3c_wait_ready(const struct device *dev, k_timeout_t timeout);

/**
 * @brief Reset-release D2D tile @p instance on the remote die and load its firmware.
 *
 * Blocks until the target answers, or @p timeout elapses. See
 * TT_GRENDEL_I3C_CMD_D2D_INIT for details.
 *
 * @param dev Grendel I3C controller device
 * @param instance D2D tile index on the remote die
 * @param timeout How long to wait for the response
 *
 * @retval 0 if the target reported TT_GRENDEL_I3C_OK
 * @retval -EIO if an I3C transfer failed or the target reported TT_GRENDEL_I3C_NOT_OK
 * @retval -ETIMEDOUT if no matching response arrived in time
 */
int tt_grendel_i3c_d2d_init(const struct device *dev, uint8_t instance, k_timeout_t timeout);

/**
 * @brief Release D2D tile @p instance's Rocket on the remote die so its loaded firmware runs.
 *
 * Blocks until the target answers, or @p timeout elapses.
 *
 * @param dev Grendel I3C controller device
 * @param instance D2D tile index on the remote die
 * @param timeout How long to wait for the response
 *
 * @retval 0 if the target reported TT_GRENDEL_I3C_OK
 * @retval -EIO if an I3C transfer failed or the target reported TT_GRENDEL_I3C_NOT_OK
 * @retval -ETIMEDOUT if no matching response arrived in time
 */
int tt_grendel_i3c_d2d_start(const struct device *dev, uint8_t instance, k_timeout_t timeout);

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_TT_GRENDEL_I3C_H_ */
