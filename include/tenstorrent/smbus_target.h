/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SMBUS_TARGET
#define SMBUS_TARGET

#include <stddef.h>
#include <stdint.h>

/**
 * @brief A list of supported SMBUS transaction types
 */
enum smbus_trans_type {
	SMBUS_TRANS_WRITE_BYTE,
	SMBUS_TRANS_READ_BYTE,
	SMBUS_TRANS_WRITE_WORD,
	SMBUS_TRANS_READ_WORD,
	SMBUS_TRANS_WRITE_BLOCK,
	SMBUS_TRANS_READ_BLOCK,
	SMBUS_TRANS_READ_WRITE_BLOCK,
};

/**
 * @brief Definition of a SMBUS receive handler
 * @details This function is invoked when the SMBUS target has data from
 *          the I2C controller to relay to the application. SMBUS receive handlers
 *          shall return 0 on success, and any other value on failure.
 */
typedef int32_t (*smbus_rcv_handler)(const uint8_t *data, uint8_t size);

/**
 * @brief Definition of a SMBUS send handler
 * @details This function is invoked when the SMBUS target requests data from the
 *          application to send to the I2C controller. SMBUS send handlers shall
 *          return 0 on success, and any other value on failure.
 */
typedef int32_t (*smbus_send_handler)(uint8_t *data, uint8_t *size);

struct smbus_cmd_def {
	enum smbus_trans_type trans_type;
	smbus_rcv_handler rcv_handler;
	smbus_send_handler send_handler;
	uint8_t pec: 1;
};

/**
 * @brief Command registration entry for table-driven SMBus setup.
 */
struct smbus_cmd_registration {
	uint8_t cmd;
	struct smbus_cmd_def def;
};

/**
 * @brief Build a generic SMBus command registration entry.
 */
#define SMBUS_CMD_ENTRY(_cmd, _pec, _trans_type, _rcv, _send)                                      \
	{                                                                                          \
		.cmd = (_cmd),                                                                     \
		.def = {.pec = (_pec),                                                             \
			.trans_type = (_trans_type),                                               \
			.rcv_handler = (_rcv),                                                     \
			.send_handler = (_send)},                                                  \
	}

/**
 * @brief Build handler-field initializers for WRITE direction.
 */
#define SMBUS_CMD_HANDLER_FIELDS_WRITE(_func) .rcv_handler = (_func), .send_handler = NULL

/**
 * @brief Build handler-field initializers for READ direction.
 */
#define SMBUS_CMD_HANDLER_FIELDS_READ(_func) .rcv_handler = NULL, .send_handler = (_func)

/**
 * @brief Build an entry by direction and transaction-kind suffix.
 * @details `_dir` must be READ or WRITE.
 * @details `_kind` must match a suffix from enum smbus_trans_type, e.g. BYTE, WORD, BLOCK.
 */
#define SMBUS_CMD_GEN_ENTRY(_cmd, _pec, _dir, _kind, _func)                                        \
	{                                                                                          \
		.cmd = (_cmd),                                                                     \
		.def = {.pec = (_pec),                                                             \
			.trans_type = SMBUS_TRANS_##_dir##_##_kind,                                \
			SMBUS_CMD_HANDLER_FIELDS_##_dir(_func)},                                   \
	}

/**
 * @brief Build an entry for SMBus block-read command handling.
 */
#define SMBUS_CMD_BLOCK_RD_ENTRY(_cmd, _pec, _send)                                                \
	SMBUS_CMD_GEN_ENTRY(_cmd, _pec, READ, BLOCK, _send)

/**
 * @brief Build an entry for SMBus block-write command handling.
 */
#define SMBUS_CMD_BLOCK_WR_ENTRY(_cmd, _pec, _rcv)                                                 \
	SMBUS_CMD_GEN_ENTRY(_cmd, _pec, WRITE, BLOCK, _rcv)

/**
 * @brief Build an entry for SMBus read-byte command handling.
 */
#define SMBUS_CMD_READ_BYTE_ENTRY(_cmd, _pec, _send)                                               \
	SMBUS_CMD_GEN_ENTRY(_cmd, _pec, READ, BYTE, _send)

/**
 * @brief Build an entry for SMBus write-byte command handling.
 */
#define SMBUS_CMD_WRITE_BYTE_ENTRY(_cmd, _pec, _rcv)                                               \
	SMBUS_CMD_GEN_ENTRY(_cmd, _pec, WRITE, BYTE, _rcv)

/**
 * @brief Build an entry for SMBus read-word command handling.
 */
#define SMBUS_CMD_READ_WORD_ENTRY(_cmd, _pec, _send)                                               \
	SMBUS_CMD_GEN_ENTRY(_cmd, _pec, READ, WORD, _send)

/**
 * @brief Build an entry for SMBus write-word command handling.
 */
#define SMBUS_CMD_WRITE_WORD_ENTRY(_cmd, _pec, _rcv)                                               \
	SMBUS_CMD_GEN_ENTRY(_cmd, _pec, WRITE, WORD, _rcv)

/**
 * @brief Build an entry for SMBus block-write-block-read command handling.
 */
#define SMBUS_CMD_BLOCK_WR_BLOCK_RD_ENTRY(_cmd, _pec, _rcv, _send)                                 \
	SMBUS_CMD_ENTRY(_cmd, _pec, SMBUS_TRANS_READ_WRITE_BLOCK, _rcv, _send)

/**
 * @brief Register the given command to the SMBUS target implementation
 * @param dev The device to register the command for.
 * @param cmd_id The command ID to register the handler for.
 * @param smbus_cmd Pointer to the smbus command to register. The memory must have
 *                  persistent storage for the duration of the time the target remains
 *                  registered to the I2C.
 */
int32_t smbus_target_register_cmd(const struct device *dev, uint8_t cmd_id,
				  const struct smbus_cmd_def *smbus_cmd);

/**
 * @brief Register a table of commands to the SMBUS target implementation.
 * @param dev The device to register commands for.
 * @param cmds Pointer to command registration table.
 * @param num_cmds Number of entries in cmds.
 * @return 0 on success, otherwise a negative error code.
 */
int32_t smbus_target_register_cmds(const struct device *dev,
				   const struct smbus_cmd_registration *cmds, size_t num_cmds);

/**
 * @brief Get total number of SMBUS target errors
 * @param dev The SMBUS target device instance.
 * @return Number of errors that went through the SMBUS ratelimited error macro.
 */
uint32_t smbus_target_get_error_count(const struct device *dev);
#endif
