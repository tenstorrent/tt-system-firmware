/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "reg.h"
#include "status_reg.h"
#include "dw_apb_i2c.h"
#include "cm2dm_msg.h"
#include "throttler.h"
#include "asic_state.h"
#include "smbus_target.h"
#include "fan_ctrl.h"

#include <stdint.h>

#include <tenstorrent/post_code.h>
#include <tenstorrent/sys_init_defines.h>
#include <tenstorrent/tt_smbus_regs.h>
#include <tenstorrent/smbus_target.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

/* DMFW to CMFW i2c interface is on I2C0 of tensix_sm */
#define CM_I2C_DM_TARGET_INST 0

/***Start of SMBus handlers***/
static const struct device *smbus_target = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(smbus_target0));

static int32_t Dm2CmSendFanSpeedHandler(const uint8_t *data, uint8_t size)
{
#ifndef CONFIG_TT_SMC_RECOVERY
	if (size != 2) {
		return -1;
	}

	uint16_t speed = sys_get_le16(data);

	DmcFanSpeedFeedback(speed);

	return 0;
#endif

	return -1;
}

static int32_t ReadByteTest(uint8_t *data, uint8_t *size)
{
	*size = 1;
	data[0] = ReadReg(STATUS_FW_SCRATCH_REG_ADDR) & 0xFF;

	return 0;
}

static int32_t WriteByteTest(const uint8_t *data, uint8_t size)
{
	if (size != 1) {
		return -1;
	}
	WriteReg(STATUS_FW_SCRATCH_REG_ADDR, size << 16 | data[0]);
	return 0;
}

static int32_t ReadWordTest(uint8_t *data, uint8_t *size)
{
	*size = 2U;

	uint32_t tmp = ReadReg(STATUS_FW_SCRATCH_REG_ADDR);

	data[0] = tmp & 0xFF;
	data[1] = (tmp >> 8) & 0xFF;

	return 0;
}

static int32_t WriteWordTest(const uint8_t *data, uint8_t size)
{
	if (size != 2) {
		return -1;
	}
	WriteReg(STATUS_FW_SCRATCH_REG_ADDR, size << 16 | data[1] << 8 | data[0]);
	return 0;
}

static int32_t BlockReadTest(uint8_t *data, uint8_t *size)
{
	*size = 4;
	uint32_t tmp = ReadReg(STATUS_FW_SCRATCH_REG_ADDR);

	memcpy(data, &tmp, 4);
	return 0;
}

int32_t BlockWriteTest(const uint8_t *data, uint8_t size)
{
	if (size != 4) {
		return -1;
	}
	uint32_t tmp;

	memcpy(&tmp, data, 4);
	WriteReg(STATUS_FW_SCRATCH_REG_ADDR, tmp);
	return 0;
}

int32_t UpdateArcStateHandler(const uint8_t *data, uint8_t size)
{
	const uint8_t sig0 = 0xDE;
	const uint8_t sig1 = 0xAF;

	if (size != 3U || data[1] != sig0 || data[2] != sig1) {
		return -1;
	}

	set_asic_state(data[0]);
	return 0;
}

/***End of SMBus handlers***/

static const struct smbus_cmd_registration smbus_cmds[] = {
	SMBUS_CMD_BLOCK_RD_ENTRY(CMFW_SMBUS_REQ, 1U, Cm2DmMsgReqSmbusHandler),
	SMBUS_CMD_WRITE_WORD_ENTRY(CMFW_SMBUS_ACK, 1U, Cm2DmMsgAckSmbusHandler),
	SMBUS_CMD_BLOCK_WR_ENTRY(CMFW_SMBUS_UPDATE_ARC_STATE, 0U, UpdateArcStateHandler),
	SMBUS_CMD_BLOCK_WR_ENTRY(CMFW_SMBUS_DM_STATIC_INFO, 1U, Dm2CmSendDataHandler),
	SMBUS_CMD_WRITE_WORD_ENTRY(CMFW_SMBUS_PING, 1U, Dm2CmPingHandler),
	SMBUS_CMD_WRITE_WORD_ENTRY(CMFW_SMBUS_FAN_SPEED, 1U, Dm2CmSendFanSpeedHandler),
	SMBUS_CMD_WRITE_WORD_ENTRY(CMFW_SMBUS_FAN_RPM, 1U, Dm2CmSendFanRPMHandler),
#ifndef CONFIG_TT_SMC_RECOVERY
	SMBUS_CMD_BLOCK_WR_BLOCK_RD_ENTRY(CMFW_SMBUS_TELEMETRY_READ, 0U, SMBusTelemRegHandler,
					  SMBusTelemDataHandler),
	SMBUS_CMD_BLOCK_WR_BLOCK_RD_ENTRY(CMFW_SMBUS_TELEMETRY_WRITE, 0U, Dm2CmWriteTelemetry,
					  Dm2CmReadControlData),
	SMBUS_CMD_WRITE_WORD_ENTRY(CMFW_SMBUS_POWER_LIMIT, 1U, Dm2CmSetBoardPowerLimit),
	SMBUS_CMD_WRITE_WORD_ENTRY(CMFW_SMBUS_POWER_INSTANT, 1U, Dm2CmSendPowerHandler),
	SMBUS_CMD_WRITE_BYTE_ENTRY(CMFW_SMBUS_TELEMETRY_READ_CRC, 1U, SMBusTelemRegHandler),
	SMBUS_CMD_BLOCK_RD_ENTRY(CMFW_SMBUS_TELEMETRY_READ_CRC_DATA, 1U, SMBusTelemDataHandler),
	SMBUS_CMD_WRITE_WORD_ENTRY(CMFW_SMBUS_THERM_TRIP_COUNT, 1U, Dm2CmSendThermTripCountHandler),
#endif
	SMBUS_CMD_BLOCK_WR_ENTRY(CMFW_SMBUS_DMC_LOG, 1U, Dm2CmDMCLogHandler),
	SMBUS_CMD_READ_BYTE_ENTRY(CMFW_SMBUS_TEST_READ, 1U, ReadByteTest),
	SMBUS_CMD_WRITE_BYTE_ENTRY(CMFW_SMBUS_TEST_WRITE, 1U, WriteByteTest),
	SMBUS_CMD_READ_WORD_ENTRY(CMFW_SMBUS_TEST_READ_WORD, 1U, ReadWordTest),
	SMBUS_CMD_WRITE_WORD_ENTRY(CMFW_SMBUS_TEST_WRITE_WORD, 1U, WriteWordTest),
	SMBUS_CMD_BLOCK_RD_ENTRY(CMFW_SMBUS_TEST_READ_BLOCK, 1U, BlockReadTest),
	SMBUS_CMD_BLOCK_WR_ENTRY(CMFW_SMBUS_TEST_WRITE_BLOCK, 1U, BlockWriteTest),
	SMBUS_CMD_BLOCK_WR_BLOCK_RD_ENTRY(CMFW_SMBUS_TEST_WRITE_BLOCK_READ_BLOCK, 1U,
					  BlockWriteTest, BlockReadTest),
	SMBUS_CMD_READ_WORD_ENTRY(CMFW_SMBUS_PING_V2, 1U, Dm2CmPingV2),
};

static int InitSmbusTarget(void)
{
	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEPB);


	if (!device_is_ready(smbus_target)) {
		printk("SMBUS target device not ready\n");
		return 0;
	}

	if (i2c_target_driver_register(smbus_target) < 0) {
		printk("Failed to register i2c target driver\n");
		return 0;
	}

	if (smbus_target_register_cmds(smbus_target, smbus_cmds, ARRAY_SIZE(smbus_cmds)) < 0) {
		printk("Failed to register SMBUS target commands\n");
		return 0;
	}

	return 0;
}
SYS_INIT_APP(InitSmbusTarget);
