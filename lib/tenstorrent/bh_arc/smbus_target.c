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

static const struct SmbusCmdDef smbus_req_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransBlockRead, .send_handler = &Cm2DmMsgReqSmbusHandler};

static const struct SmbusCmdDef smbus_ack_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransWriteWord, .rcv_handler = &Cm2DmMsgAckSmbusHandler};

static const struct SmbusCmdDef smbus_update_arc_state_cmd_def = {
	.pec = 0U, .trans_type = kSmbusTransBlockWrite, .rcv_handler = &UpdateArcStateHandler};

static const struct SmbusCmdDef smbus_dm_static_info_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransBlockWrite, .rcv_handler = &Dm2CmSendDataHandler};

static const struct SmbusCmdDef smbus_ping_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransWriteWord, .rcv_handler = &Dm2CmPingHandler};

static const struct SmbusCmdDef smbus_fan_speed_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransWriteWord, .rcv_handler = &Dm2CmSendFanSpeedHandler};

static const struct SmbusCmdDef smbus_fan_rpm_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransWriteWord, .rcv_handler = &Dm2CmSendFanRPMHandler};

#ifndef CONFIG_TT_SMC_RECOVERY
static const struct SmbusCmdDef smbus_telem_read_cmd_def = {.pec = 0U,
							    .trans_type =
								    kSmbusTransBlockWriteBlockRead,
							    .rcv_handler = SMBusTelemRegHandler,
							    .send_handler = SMBusTelemDataHandler};

static const struct SmbusCmdDef smbus_telem_write_cmd_def = {.pec = 0U,
							     .trans_type =
								     kSmbusTransBlockWriteBlockRead,
							     .rcv_handler = Dm2CmWriteTelemetry,
							     .send_handler = Dm2CmReadControlData};

static const struct SmbusCmdDef smbus_power_limit_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransWriteWord, .rcv_handler = &Dm2CmSetBoardPowerLimit};

static const struct SmbusCmdDef smbus_power_instant_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransWriteWord, .rcv_handler = &Dm2CmSendPowerHandler};

static const struct SmbusCmdDef smbus_telem_reg_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransWriteByte, .rcv_handler = &SMBusTelemRegHandler};

static const struct SmbusCmdDef smbus_telem_data_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransBlockRead, .send_handler = &SMBusTelemDataHandler};

static const struct SmbusCmdDef smbus_therm_trip_count_cmd_def = {
	.pec = 1U,
	.trans_type = kSmbusTransWriteWord,
	.rcv_handler = &Dm2CmSendThermTripCountHandler};

#endif /*CONFIG_TT_SMC_RECOVERY*/
static const struct SmbusCmdDef smbus_dmc_log_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransBlockWrite, .rcv_handler = &Dm2CmDMCLogHandler};

static const struct SmbusCmdDef smbus_test_read_byte_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransReadByte, .send_handler = &ReadByteTest};

static const struct SmbusCmdDef smbus_test_write_byte_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransWriteByte, .rcv_handler = &WriteByteTest};

static const struct SmbusCmdDef smbus_test_read_word_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransReadWord, .send_handler = &ReadWordTest};

static const struct SmbusCmdDef smbus_test_write_word_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransWriteWord, .rcv_handler = &WriteWordTest};

static const struct SmbusCmdDef smbus_block_write_block_read_test = {
	.pec = 1U,
	.trans_type = kSmbusTransBlockWriteBlockRead,
	.rcv_handler = &BlockWriteTest,
	.send_handler = BlockReadTest};

static const struct SmbusCmdDef smbus_test_read_block_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransBlockRead, .send_handler = &BlockReadTest};

static const struct SmbusCmdDef smbus_test_write_block_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransBlockWrite, .rcv_handler = &BlockWriteTest};

static const struct SmbusCmdDef smbus_ping_v2_cmd_def = {
	.pec = 1U, .trans_type = kSmbusTransReadWord, .send_handler = &Dm2CmPingV2};

static int InitSmbusTarget(void)
{
	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEPB);

	if (IS_ENABLED(CONFIG_ARC)) {
		I2CInitGPIO(CM_I2C_DM_TARGET_INST);
	}

	if (!device_is_ready(smbus_target)) {
		printk("SMBUS target device not ready\n");
		return 0;
	}

	if (i2c_target_driver_register(smbus_target) < 0) {
		printk("Failed to register i2c target driver\n");
		return 0;
	}

	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_REQ, &smbus_req_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_ACK, &smbus_ack_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_UPDATE_ARC_STATE,
				  &smbus_update_arc_state_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_DM_STATIC_INFO,
				  &smbus_dm_static_info_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_PING, &smbus_ping_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_FAN_SPEED, &smbus_fan_speed_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_FAN_RPM, &smbus_fan_rpm_cmd_def);
#ifndef CONFIG_TT_SMC_RECOVERY
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_TELEMETRY_READ,
				  &smbus_telem_read_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_TELEMETRY_WRITE,
				  &smbus_telem_write_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_POWER_LIMIT, &smbus_power_limit_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_POWER_INSTANT,
				  &smbus_power_instant_cmd_def);
	smbus_target_register_cmd(smbus_target, 0x26, &smbus_telem_reg_cmd_def);
	smbus_target_register_cmd(smbus_target, 0x27, &smbus_telem_data_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_THERM_TRIP_COUNT,
				  &smbus_therm_trip_count_cmd_def);
#endif
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_DMC_LOG, &smbus_dmc_log_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_TEST_READ,
				  &smbus_test_read_byte_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_TEST_WRITE,
				  &smbus_test_write_byte_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_TEST_READ_WORD,
				  &smbus_test_read_word_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_TEST_WRITE_WORD,
				  &smbus_test_write_word_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_TEST_READ_BLOCK,
				  &smbus_test_read_block_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_TEST_WRITE_BLOCK,
				  &smbus_test_write_block_cmd_def);
	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_TEST_WRITE_BLOCK_READ_BLOCK,
				  &smbus_block_write_block_read_test);

	smbus_target_register_cmd(smbus_target, CMFW_SMBUS_PING_V2, &smbus_ping_v2_cmd_def);
	return 0;
}
SYS_INIT_APP(InitSmbusTarget);
