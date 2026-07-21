/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zephyr_smbus_target

#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/atomic.h>
#include <tenstorrent/smbus_target.h>

#define LOG_LEVEL CONFIG_I2C_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(i2c_target);

#define SMBUS_LOG_ERR_RATELIMIT(data, fmt, ...)                                                    \
	do {                                                                                       \
		atomic_inc(&(data)->error_count);                                                  \
		LOG_ERR_RATELIMIT(fmt, ##__VA_ARGS__);                                             \
	} while (0)

typedef enum {
	kSmbusStateIdle,
	kSmbusStateCmd,
	kSmbusStateRcvData,
	kSmbusStateRcvPec,
	kSmbusStateSendData,
	kSmbusStateSendPec,
	kSmbusStateWaitIdle, /* After transactions finish, and in error conditions */
} SmbusState;

struct smbus_target_data {
	struct i2c_target_config config;
	const struct SmbusCmdDef *cmd_defs[256];
	atomic_t error_count;
	SmbusState state;
	uint8_t command;
	uint8_t blocksize_r;
	uint8_t blocksize_w;
	uint8_t rcv_index;
	uint8_t send_index;
	uint8_t received_data[CONFIG_SMBUS_MAX_MSG_SIZE];
	uint8_t send_data[CONFIG_SMBUS_MAX_MSG_SIZE];
};

struct smbus_target_config {
	struct i2c_dt_spec bus;
};

static const struct SmbusCmdDef *get_cmd_def(struct smbus_target_data *smbus_data, uint8_t cmd)
{
	return smbus_data->cmd_defs[cmd];
}

static inline uint8_t pec_crc_8(uint8_t crc, uint8_t data)
{
	return crc8(&data, 1, 0x7, crc, false);
}

static int32_t smbus_target_register(const struct device *dev)
{
	const struct smbus_target_config *cfg = dev->config;
	struct smbus_target_data *data = dev->data;

	return i2c_target_register(cfg->bus.bus, &data->config);
}

static int32_t smbus_target_unregister(const struct device *dev)
{
	const struct smbus_target_config *cfg = dev->config;
	struct smbus_target_data *data = dev->data;

	return i2c_target_unregister(cfg->bus.bus, &data->config);
}

static int smbus_write_handler(struct i2c_target_config *config, uint8_t val)
{
	int32_t ret = 0;
	struct smbus_target_data *smbus_data =
		CONTAINER_OF(config, struct smbus_target_data, config);
	const struct SmbusCmdDef *curr_cmd = get_cmd_def(smbus_data, smbus_data->command);

	if (smbus_data->state == kSmbusStateIdle) {
		/*Log something like WriteReg(I2C0_TARGET_DEBUG_STATE_REG_ADDR, 0xc0de1030); */
		smbus_data->command = val;
		curr_cmd = get_cmd_def(smbus_data, smbus_data->command);
		if (!curr_cmd) {
			/* Command not implemented */
			SMBUS_LOG_ERR_RATELIMIT(smbus_data, "SMBUS: command 0x%02x not implemented",
						val);
			smbus_data->state = kSmbusStateWaitIdle;
			return -1;
		}
		smbus_data->state = kSmbusStateCmd;
	} else if (smbus_data->state == kSmbusStateCmd) {
		/*Log something like WriteReg(I2C0_TARGET_DEBUG_STATE_REG_ADDR, 0xc0de1040); */
		switch (curr_cmd->trans_type) {
		case kSmbusTransBlockWrite:
		case kSmbusTransBlockWriteBlockRead:
			smbus_data->blocksize_w = val;
			if (smbus_data->blocksize_w > CONFIG_SMBUS_MAX_MSG_SIZE) {
				SMBUS_LOG_ERR_RATELIMIT(
					smbus_data,
					"SMBUS: oversized block write: %d bytes (max %d)",
					smbus_data->blocksize_w, CONFIG_SMBUS_MAX_MSG_SIZE);
				/* block size too big */
				smbus_data->state = kSmbusStateWaitIdle;
				ret = -1;
			} else {
				smbus_data->state = kSmbusStateRcvData;
			}
			break;
		case kSmbusTransWriteByte:
			smbus_data->blocksize_w = 1;
			smbus_data->received_data[smbus_data->rcv_index++] = val;

			if (1U == curr_cmd->pec) {
				smbus_data->state = kSmbusStateRcvPec;
			} else {
				ret = curr_cmd->rcv_handler(smbus_data->received_data,
							    smbus_data->blocksize_w);

				smbus_data->state = kSmbusStateWaitIdle;
			}
			break;
		case kSmbusTransWriteWord:
			smbus_data->blocksize_w = 2;
			smbus_data->received_data[smbus_data->rcv_index++] = val;
			smbus_data->state = kSmbusStateRcvData;
			break;
		default:
			/* Error, invalid command for write */
			SMBUS_LOG_ERR_RATELIMIT(smbus_data,
						"SMBUS: invalid transaction type %d for write",
						curr_cmd->trans_type);
			smbus_data->state = kSmbusStateWaitIdle;
			ret = -1;
		}
	} else if (smbus_data->state == kSmbusStateRcvData) {
		/*Log something like WriteReg(I2C0_TARGET_DEBUG_STATE_REG_ADDR, 0xc0de1050); */
		smbus_data->received_data[smbus_data->rcv_index++] = val;
		if (smbus_data->rcv_index == smbus_data->blocksize_w) {
			if (1U == curr_cmd->pec &&
			    (curr_cmd->trans_type != kSmbusTransBlockWriteBlockRead)) {
				smbus_data->state = kSmbusStateRcvPec;
			} else {
				ret = curr_cmd->rcv_handler(smbus_data->received_data,
							    smbus_data->blocksize_w);
				if (ret == 0 &&
				    curr_cmd->trans_type == kSmbusTransBlockWriteBlockRead) {
					smbus_data->state = kSmbusStateCmd;
				} else if (ret != 0) {
					SMBUS_LOG_ERR_RATELIMIT(
						smbus_data,
						"SMBUS: receive handler error (cmd 0x%02x)",
						smbus_data->command);
					smbus_data->state = kSmbusStateWaitIdle;
				} else {
					smbus_data->state = kSmbusStateWaitIdle;
				}
			}
		}
	} else if (smbus_data->state == kSmbusStateRcvPec) {
		/*Log something like WriteReg(I2C0_TARGET_DEBUG_STATE_REG_ADDR, 0xc0de1060);*/
		uint8_t rcv_pec = val;

		/* Calculate the PEC */
		uint8_t pec = 0;

		pec = pec_crc_8(pec, smbus_data->config.address << 1 |
					     I2C_MSG_WRITE); /* start address byte */
		pec = pec_crc_8(pec, smbus_data->command);
		if (curr_cmd->trans_type == kSmbusTransBlockWrite) {
			pec = pec_crc_8(pec, smbus_data->blocksize_w);
		}
		for (int i = 0; i < smbus_data->blocksize_w; i++) {
			pec = pec_crc_8(pec, smbus_data->received_data[i]);
		}

		if (pec != rcv_pec) {
			SMBUS_LOG_ERR_RATELIMIT(
				smbus_data,
				"SMBUS: PEC mismatch (cmd 0x%02x): expected 0x%02x, got 0x%02x",
				smbus_data->command, pec, rcv_pec);
			smbus_data->state = kSmbusStateWaitIdle;
			return -1;
		}
		ret = curr_cmd->rcv_handler(smbus_data->received_data, smbus_data->blocksize_w);

		if (ret == 0 && curr_cmd->trans_type == kSmbusTransBlockWriteBlockRead) {
			smbus_data->state = kSmbusStateCmd;
		} else if (ret != 0) {
			SMBUS_LOG_ERR_RATELIMIT(smbus_data,
						"SMBUS: PEC receive handler error (cmd 0x%02x)",
						smbus_data->command);
			smbus_data->state = kSmbusStateWaitIdle;
		} else {
			smbus_data->state = kSmbusStateWaitIdle;
		}
	} else {
		/* Invalid state for write handler */
		SMBUS_LOG_ERR_RATELIMIT(smbus_data,
					"SMBUS: write handler called in invalid state %d",
					smbus_data->state);
		smbus_data->state = kSmbusStateWaitIdle;
		ret = -1;
	}
	return ret;
}

static int32_t smbus_read_handler(struct i2c_target_config *config, uint8_t *val)
{
	struct smbus_target_data *smbus_data =
		CONTAINER_OF(config, struct smbus_target_data, config);

	const struct SmbusCmdDef *curr_cmd = get_cmd_def(smbus_data, smbus_data->command);

	if (smbus_data->state == kSmbusStateCmd) {
		/* Log something like WriteReg(I2C0_TARGET_DEBUG_STATE_REG_ADDR, 0xc0de0010); */

		/* Calculate blocksize for different types of commands */
		switch (curr_cmd->trans_type) {
		case kSmbusTransBlockRead:
		case kSmbusTransBlockWriteBlockRead:
		case kSmbusTransReadByte:
		case kSmbusTransReadWord:
			break;
		default:
			/* Error, invalid command for read */
			SMBUS_LOG_ERR_RATELIMIT(smbus_data,
						"SMBUS: invalid transaction type %d for read",
						curr_cmd->trans_type);
			smbus_data->state = kSmbusStateWaitIdle;
			*val = 0xFF;
			return -1;
		}
		/* Call the send handler to get the data */
		if (curr_cmd->send_handler(smbus_data->send_data, &smbus_data->blocksize_r)) {
			/* Send handler returned error */
			SMBUS_LOG_ERR_RATELIMIT(smbus_data,
						"SMBUS: send handler error (cmd 0x%02x)",
						smbus_data->command);
			smbus_data->state = kSmbusStateWaitIdle;
			*val = 0xFF;
			return -1;
		}
		/* Send the correct data for different types of commands */
		switch (curr_cmd->trans_type) {
		case kSmbusTransBlockWriteBlockRead:
		case kSmbusTransBlockRead:
			/*Log something like WriteReg(I2C0_TARGET_DEBUG_STATE_REG_ADDR,
			 * 0xc0de0030);
			 */
			*val = smbus_data->blocksize_r;
			smbus_data->state = kSmbusStateSendData;
			break;
		case kSmbusTransReadByte:
			*val = smbus_data->send_data[smbus_data->send_index++];
			smbus_data->state =
				curr_cmd->pec ? kSmbusStateSendPec : kSmbusStateWaitIdle;
			break;
		case kSmbusTransReadWord:
			*val = smbus_data->send_data[smbus_data->send_index++];
			smbus_data->state = kSmbusStateSendData;
			break;
		default:
			/* Error, invalid command for read */
			SMBUS_LOG_ERR_RATELIMIT(
				smbus_data, "SMBUS: invalid transaction type %d for read response",
				curr_cmd->trans_type);
			smbus_data->state = kSmbusStateWaitIdle;
			*val = 0xFF;
			return -1;
		}
	} else if (smbus_data->state == kSmbusStateSendData) {
		/* Log something like WriteReg(I2C0_TARGET_DEBUG_STATE_REG_ADDR, 0xc0de0050); */
		*val = smbus_data->send_data[smbus_data->send_index++];
		if (smbus_data->send_index == smbus_data->blocksize_r) {
			smbus_data->state =
				curr_cmd->pec ? kSmbusStateSendPec : kSmbusStateWaitIdle;
		}
	} else if (smbus_data->state == kSmbusStateSendPec) {
		/* Log something like WriteReg(I2C0_TARGET_DEBUG_STATE_REG_ADDR, 0xc0de0060);*/
		/* Calculate and send PEC. This is a read-type operation, so it starts with a
		 * sequence of writes, then some reads.
		 */

		uint8_t pec = 0;

		pec = pec_crc_8(pec, smbus_data->config.address << 1 |
					     I2C_MSG_WRITE); /* start address byte */
		pec = pec_crc_8(pec, smbus_data->command);

		if (curr_cmd->trans_type == kSmbusTransBlockWriteBlockRead) {
			pec = pec_crc_8(pec, smbus_data->blocksize_w);
		}
		/* any received data */
		for (int i = 0; i < smbus_data->rcv_index; i++) {
			pec = pec_crc_8(pec, smbus_data->received_data[i]);
		}

		pec = pec_crc_8(pec, smbus_data->config.address << 1 |
					     I2C_MSG_READ); /* restart address byte */

		/* sent data */
		if (curr_cmd->trans_type == kSmbusTransBlockRead ||
		    curr_cmd->trans_type == kSmbusTransBlockWriteBlockRead) {
			pec = pec_crc_8(pec, smbus_data->blocksize_r);
		}
		for (int i = 0; i < smbus_data->blocksize_r; i++) {
			pec = pec_crc_8(pec, smbus_data->send_data[i]);
		}

		*val = pec;
		smbus_data->state = kSmbusStateWaitIdle;
	} else {
		/* Invalid state for read handler */
		SMBUS_LOG_ERR_RATELIMIT(smbus_data,
					"SMBUS: read handler called in invalid state %d",
					smbus_data->state);
		smbus_data->state = kSmbusStateWaitIdle;
		*val = 0xFF;
		return -1;
	}
	return 0;
}

uint32_t smbus_target_get_error_count(const struct device *dev)
{
	if (dev == NULL || dev->data == NULL) {
		return 0;
	}

	struct smbus_target_data *data = dev->data;

	return (uint32_t)atomic_get(&data->error_count);
}

static void reset_state_machine(struct smbus_target_data *smbus_data)
{
	smbus_data->state = kSmbusStateIdle;
	smbus_data->command = 0;
	smbus_data->blocksize_r = 0;
	smbus_data->blocksize_w = 0;
	smbus_data->rcv_index = 0;
	smbus_data->send_index = 0;

	/* Don't erase data buffers for efficiency */
}

static int32_t smbus_stop_handler(struct i2c_target_config *config)
{
	reset_state_machine(CONTAINER_OF(config, struct smbus_target_data, config));
	return 0;
}

/*Unfortunately, there is a delta between how I2C target controllers treat
 *Incoming data streams. The i2c_emul controller requires this CB be implemented,
 *otherwise it will trip on a NULL ptr dereference.
 */
static int32_t smbus_write_requested(struct i2c_target_config *config)
{
	reset_state_machine(CONTAINER_OF(config, struct smbus_target_data, config));
	return 0;
}

static DEVICE_API(i2c_target, api_funcs) = {
	.driver_register = smbus_target_register,
	.driver_unregister = smbus_target_unregister,
};

const struct i2c_target_callbacks smbus_target_cb_impl = {
	.write_requested = &smbus_write_requested,
	.write_received = &smbus_write_handler,
	.read_requested = &smbus_read_handler,
	.read_processed = &smbus_read_handler,
	.stop = &smbus_stop_handler,
};

static int32_t smbus_target_init(const struct device *dev)
{
	struct smbus_target_data *data = dev->data;
	const struct smbus_target_config *cfg = dev->config;

	if (!device_is_ready(cfg->bus.bus)) {
		LOG_ERR("I2C controller device not ready");
		return -ENODEV;
	}

	data->config.address = cfg->bus.addr;
	data->config.callbacks = &smbus_target_cb_impl;
	atomic_set(&data->error_count, 0);

	return 0;
}

int32_t smbus_target_register_cmd(const struct device *dev, uint8_t cmd_id,
				  const struct SmbusCmdDef *smbus_cmd)
{
	struct smbus_target_data *data = dev->data;

	if (!device_is_ready(dev)) {
		LOG_ERR("SMBUS device not ready");
		return -ENODEV;
	}

	data->cmd_defs[cmd_id] = smbus_cmd;
	return 0;
}

#define SMBUS_TARGET_INIT(inst)                                                                    \
	static struct smbus_target_data smbus_target_##inst##_dev_data = {0};                      \
	static const struct smbus_target_config smbus_target_##inst##_cfg = {                      \
		.bus = I2C_DT_SPEC_INST_GET(inst),                                                 \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, &smbus_target_init, NULL, &smbus_target_##inst##_dev_data,     \
			      &smbus_target_##inst##_cfg, POST_KERNEL,                             \
			      CONFIG_I2C_TARGET_INIT_PRIORITY, &api_funcs);

DT_INST_FOREACH_STATUS_OKAY(SMBUS_TARGET_INIT)
