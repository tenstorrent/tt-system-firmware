/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "avs.h"
#include "dw_apb_i2c.h"
#include "init.h"
#include "regulator.h"
#include "regulator_config.h"
#include "status_reg.h"
#include "timer.h"

#include <math.h>  /* for ldexp */
#include <float.h> /* for FLT_MAX */
#include <stdint.h>

#include <tenstorrent/smc_msg.h>
#include <tenstorrent/msgqueue.h>
#include <tenstorrent/post_code.h>
#include <tenstorrent/sys_init_defines.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/misc/bh_fwtable.h>

#define LINEAR_FORMAT_CONSTANT (1 << 9)
#define SCALE_LOOP             0.335f

/* I2C constants */
#define PMBUS_MST_ID 1

/* PMBus Spec constants */
#define MFR_CTRL_OPS                   0xD2
#define MFR_CTRL_OPS_DATA_BYTE_SIZE    1
#define VOUT_COMMAND                   0x21
#define VOUT_COMMAND_DATA_BYTE_SIZE    2
#define VOUT_SCALE_LOOP                0x29
#define VOUT_SCALE_LOOP_DATA_BYTE_SIZE 2
#define READ_VOUT                      0x8B
#define READ_VOUT_DATA_BYTE_SIZE       2
#define READ_IOUT                      0x8C
#define READ_IOUT_DATA_BYTE_SIZE       2
#define READ_POUT                      0x96
#define READ_POUT_DATA_BYTE_SIZE       2
#define OPERATION                      0x1
#define OPERATION_DATA_BYTE_SIZE       1
#define PMBUS_CMD_BYTE_SIZE            1
#define PMBUS_FLIP_BYTES               0

/* VR feedback resistors */
#define GDDR_VDDR_FB1         0.422
#define GDDR_VDDR_FB2         1.0
#define CB_GDDR_VDDR_FB1      1.37
#define CB_GDDR_VDDR_FB2      4.32
#define SCRAPPY_GDDR_VDDR_FB1 1.07
#define SCRAPPY_GDDR_VDDR_FB2 3.48

struct OperationBits {
	uint8_t reserved: 1;
	uint8_t transition_control: 1;
	uint8_t margin_fault_response: 2;

	enum VoltageCmdSource voltage_command_source: 2;
	uint8_t turn_off_behaviour: 1;
	uint8_t on_off_state: 1;
};
LOG_MODULE_REGISTER(regulator);

/* The default value is the regulator default */
static uint8_t vout_cmd_source = VoutCommand;
static const struct device *const fwtable_dev = DEVICE_DT_GET(DT_NODELABEL(fwtable));

static float ConvertLinear11ToFloat(uint16_t value)
{
	int16_t exponent = (value >> 11) & 0x1f;
	uint16_t mantissa = value & 0x7ff;

	if (exponent >> 4 == 1) { /* sign extension if negative */
		exponent |= ~0x1F;
	}

	return ldexp(mantissa, exponent);
}

/* The function returns the core current in A. */
float GetVcoreCurrent(void)
{
	I2CInit(I2CMst, P0V8_VCORE_ADDR, I2CFastMode, PMBUS_MST_ID);
	uint16_t iout;

	I2CReadBytes(PMBUS_MST_ID, READ_IOUT, PMBUS_CMD_BYTE_SIZE, (uint8_t *)&iout,
		     READ_IOUT_DATA_BYTE_SIZE, PMBUS_FLIP_BYTES);
	return ConvertLinear11ToFloat(iout);
}
float getVddioCurrent(void)
{
	I2CInit(I2CMst, GDDRIO_WEST_ADDR, I2CFastMode, PMBUS_MST_ID);
	uint16_t iout;

	I2CReadBytes(PMBUS_MST_ID, READ_IOUT, PMBUS_CMD_BYTE_SIZE, (uint8_t *)&iout,
		     READ_IOUT_DATA_BYTE_SIZE, PMBUS_FLIP_BYTES);
	
	uint16_t cleaned_bits_iout = iout & (0x3FFF);
	float iout_amps = cleaned_bits_iout * .0625f;

	return iout_amps;
}
/* The function returns the core power in W. */
float GetVcorePower(void)
{
	I2CInit(I2CMst, P0V8_VCORE_ADDR, I2CFastMode, PMBUS_MST_ID);
	uint16_t pout;

	I2CReadBytes(PMBUS_MST_ID, READ_POUT, PMBUS_CMD_BYTE_SIZE, (uint8_t *)&pout,
		     READ_POUT_DATA_BYTE_SIZE, PMBUS_FLIP_BYTES);
	return ConvertLinear11ToFloat(pout);
}

static void set_max20730(uint32_t slave_addr, uint32_t voltage_in_mv, float rfb1, float rfb2)
{
	I2CInit(I2CMst, slave_addr, I2CFastMode, PMBUS_MST_ID);
	float vref = voltage_in_mv / (1 + rfb1 / rfb2);
	uint16_t vout_cmd = vref * LINEAR_FORMAT_CONSTANT * 0.001f;

	I2CWriteBytes(PMBUS_MST_ID, VOUT_COMMAND, PMBUS_CMD_BYTE_SIZE, (uint8_t *)&vout_cmd,
		      VOUT_COMMAND_DATA_BYTE_SIZE);

	/* delay to flush i2c transaction and voltage change */
	WaitUs(250);
}



static void set_mpm3695(uint32_t slave_addr, uint32_t voltage_in_mv, float rfb1, float rfb2)
{
	I2CInit(I2CMst, slave_addr, I2CFastMode, PMBUS_MST_ID);
	uint16_t vout_cmd = voltage_in_mv * 0.5f / SCALE_LOOP / (1 + rfb1 / rfb2);

	I2CWriteBytes(PMBUS_MST_ID, VOUT_COMMAND, PMBUS_CMD_BYTE_SIZE, (uint8_t *)&vout_cmd,
		      VOUT_COMMAND_DATA_BYTE_SIZE);

	/* delay to flush i2c transaction and voltage change */
	WaitUs(250);
}

/* Set MAX20816 voltage using I2C, MAX20816 is used for Vcore and Vcorem */
static void i2c_set_max20816(uint32_t slave_addr, uint32_t voltage_in_mv)
{
	I2CInit(I2CMst, slave_addr, I2CFastMode, PMBUS_MST_ID);
	uint16_t vout_cmd = 2 * voltage_in_mv;

	I2CWriteBytes(PMBUS_MST_ID, VOUT_COMMAND, PMBUS_CMD_BYTE_SIZE, (uint8_t *)&vout_cmd,
		      VOUT_COMMAND_DATA_BYTE_SIZE);

	/* 100us to flush the tx of i2c + 150us to cover voltage switch from 0.65V to 0.95V with
	 * 50us of margin
	 */
	WaitUs(250);
}

/* Returns MAX20816 output volage in mV. */
static float i2c_get_max20816(uint32_t slave_addr)
{
	I2CInit(I2CMst, slave_addr, I2CFastMode, PMBUS_MST_ID);
	uint16_t vout_cmd = 0;

	I2CReadBytes(PMBUS_MST_ID, READ_VOUT, PMBUS_CMD_BYTE_SIZE, (uint8_t *)&vout_cmd,
		     READ_VOUT_DATA_BYTE_SIZE, PMBUS_FLIP_BYTES);

	return vout_cmd * 0.5f;
}

void set_vcore(uint32_t voltage_in_mv)
{
	if (vout_cmd_source == AVSVoutCommand) {
		AVSWriteVoltage(voltage_in_mv, AVS_VCORE_RAIL);
	} else {
		i2c_set_max20816(P0V8_VCORE_ADDR, voltage_in_mv);
	}
}

uint32_t get_vcore(void)
{
	return i2c_get_max20816(P0V8_VCORE_ADDR);
}

void set_vcorem(uint32_t voltage_in_mv)
{
	i2c_set_max20816(P0V8_VCOREM_ADDR, voltage_in_mv);
}

uint32_t get_vcorem(void)
{
	return i2c_get_max20816(P0V8_VCOREM_ADDR);
}

/* Set GDDR VDDR voltage for corner parts before DRAM training */
void set_gddr_vddr(PcbType board_type, uint32_t voltage_in_mv)
{
	if (board_type == PcbTypeOrionSLT) {
		set_max20730(CB_GDDR_VDDR_WEST_ADDR, voltage_in_mv, CB_GDDR_VDDR_FB1,
			     CB_GDDR_VDDR_FB2);
		set_max20730(CB_GDDR_VDDR_EAST_ADDR, voltage_in_mv, CB_GDDR_VDDR_FB1,
			     CB_GDDR_VDDR_FB2);
	} else {
		set_mpm3695(GDDR_VDDR_ADDR, voltage_in_mv, GDDR_VDDR_FB1, GDDR_VDDR_FB2);
	}
}

void SwitchVoutControl(enum VoltageCmdSource source)
{
	I2CInit(I2CMst, P0V8_VCORE_ADDR, I2CFastMode, PMBUS_MST_ID);
	struct OperationBits operation;

	I2CReadBytes(PMBUS_MST_ID, OPERATION, PMBUS_CMD_BYTE_SIZE, (uint8_t *)&operation,
		     OPERATION_DATA_BYTE_SIZE, PMBUS_FLIP_BYTES);
	operation.transition_control =
		1; /* copy vout command when control is passed from AVSBus to PMBus */
	operation.voltage_command_source = source;
	I2CWriteBytes(PMBUS_MST_ID, OPERATION, PMBUS_CMD_BYTE_SIZE, (uint8_t *)&operation,
		      OPERATION_DATA_BYTE_SIZE);

	/* 100us to flush the tx of i2c */
	WaitUs(100);
	vout_cmd_source = source;
}

uint32_t RegulatorInit(PcbType board_type)
{
	uint32_t aggregate_i2c_errors = 0;
	uint32_t i2c_error = 0;

	const struct board_regulators_config *regulators_config = NULL;

	if (board_type == PcbTypeP150) {
		regulators_config = &p150_regulators_config;
	} else if (board_type == PcbTypeP300) {
		if (tt_bh_fwtable_is_p300_left_chip()) {
			regulators_config = &p300_left_regulators_config;
		} else {
			regulators_config = &p300_right_regulators_config;
		}
	} else if (board_type == PcbTypeUBB) {
		regulators_config = &ubb_regulators_config;
	} else if (board_type == PcbTypeOrionSLT) {
		/* Do nothing, Orion SLT regulators are manually programmed */
	} else {
		LOG_ERR("Unsupported board type %d", board_type);
		return -ENOTSUP;
	}
	if (regulators_config) {
		for (uint32_t i = 0; i < regulators_config->count; i++) {
			const struct regulator_config *regulator_config =
				regulators_config->regulator_config + i;

			I2CInit(I2CMst, regulator_config->address, I2CFastMode, PMBUS_MST_ID);

			for (uint32_t j = 0; j < regulator_config->count; j++) {
				const struct regulator_data *regulator_data =
					&regulator_config->regulator_data[j];

				LOG_DBG("Regulator %#x init on cmd %#x", regulator_config->address,
					regulator_data->cmd);

				i2c_error = I2CRMWV(PMBUS_MST_ID, regulator_data->cmd,
						    PMBUS_CMD_BYTE_SIZE, regulator_data->data,
						    regulator_data->mask, regulator_data->size);

				if (i2c_error) {
					LOG_WRN("Regulator %#x init retried on cmd %#x "
						"with error %#x",
						regulator_config->address, regulator_data->cmd,
						i2c_error);

					/* First, try a bus recovery */
					I2CRecoverBus(PMBUS_MST_ID);
					/* Retry once */
					i2c_error =
						I2CRMWV(PMBUS_MST_ID, regulator_data->cmd,
							PMBUS_CMD_BYTE_SIZE, regulator_data->data,
							regulator_data->mask, regulator_data->size);
					if (i2c_error) {
						LOG_ERR("Regulator init failed on cmd %#x "
							"with error %#x",
							regulator_data->cmd, i2c_error);
						aggregate_i2c_errors |= i2c_error;
					} else {
						LOG_INF("Regulator init succeeded on cmd %#x",
							regulator_data->cmd);
					}
				}
			}
		}
	}
	return aggregate_i2c_errors;
}

/**
 * @brief Handler for @ref TT_SMC_MSG_SET_VOLTAGE messages
 *
 * @details Sets the voltage on the specified regulator via I2C. The request should contain
 *          the I2C slave address and the voltage value in millivolts.
 *
 * @param request Pointer to the host request message, use request->set_voltage for structured
 *                access
 * @param response Pointer to the response message to be sent back to host
 *
 * @return 0 on success
 * @return non-zero on error
 *
 * @see set_voltage_rqst
 */
static uint8_t set_voltage_handler(const union request *request, struct response *response)
{
	uint32_t slave_addr = request->set_voltage.slave_addr;
	uint32_t voltage_in_mv = request->set_voltage.voltage_in_mv;

	switch (slave_addr) {
	case P0V8_VCORE_ADDR:
		set_vcore(voltage_in_mv);
		return 0;
	case P0V8_VCOREM_ADDR:
		set_vcorem(voltage_in_mv);
		return 0;
	default:
		return 1;
	}
}

/**
 * @brief Handler for @ref TT_SMC_MSG_GET_VOLTAGE messages
 *
 * @details Reads the current voltage from the specified regulator via I2C and returns
 *          it in the response message.
 *
 * @param request Pointer to the host request message, use request->get_voltage for structured
 *                access
 * @param response Pointer to the response message to be sent back to host, will contain:
 *                 - data[1]: Current voltage reading in millivolts
 *
 * @return 0 on success
 * @return non-zero on error
 *
 * @see get_voltage_rqst
 */
static uint8_t get_voltage_handler(const union request *request, struct response *response)
{
	uint32_t slave_addr = request->get_voltage.slave_addr;

	switch (slave_addr) {
	case P0V8_VCORE_ADDR:
		response->data[1] = get_vcore();
		return 0;
	case P0V8_VCOREM_ADDR:
		response->data[1] = get_vcorem();
		return 0;
	default:
		return 1;
	}
}

/**
 * @brief Handler for @ref TT_SMC_MSG_SWITCH_VOUT_CONTROL messages
 *
 * @details Switches the VOUT control source for voltage regulators. This allows
 *          switching between different control methods.
 *
 * @param request Pointer to the host request message, use request->switch_vout_control for
 *                structured access
 * @param response Pointer to the response message to be sent back to host
 *
 * @return 0 on success
 * @return non-zero on error
 *
 * @see switch_vout_control_rqst
 */
static uint8_t switch_vout_control_handler(const union request *request, struct response *response)
{
	uint32_t source = request->switch_vout_control.source;

	SwitchVoutControl(source);
	return 0;
}

REGISTER_MESSAGE(TT_SMC_MSG_SET_VOLTAGE, set_voltage_handler);
REGISTER_MESSAGE(TT_SMC_MSG_GET_VOLTAGE, get_voltage_handler);
REGISTER_MESSAGE(TT_SMC_MSG_SWITCH_VOUT_CONTROL, switch_vout_control_handler);

static int regulator_init(void)
{
	int ret;

	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEPC);

	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY) || !IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	ret = (int)RegulatorInit(tt_bh_fwtable_get_pcb_type(fwtable_dev));
	if (ret != 0) {
		record_init_failure(INIT_STAGE_REGULATOR);
		return -EIO;
	}

	return 0;
}
SYS_INIT_APP(regulator_init);
