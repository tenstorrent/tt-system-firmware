/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "avs.h"
#include "regulator.h"

#include <tenstorrent/sys_init_defines.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control/clock_control_tt_bh.h>
#include <zephyr/drivers/clock_control.h>

static const struct device *const pll_dev_1 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(pll1));

#include "timer.h"
#include <tenstorrent/reg.h>

#define APB2AVSBUS_AVS_INTERRUPT_MASK_REG_ADDR 0x80100034
#define APB2AVSBUS_AVS_CFG_1_REG_ADDR          0x80100054
#define APB2AVSBUS_AVS_FIFOS_STATUS_REG_ADDR   0x80100028
#define APB2AVSBUS_AVS_CFG_0_REG_ADDR          0x80100050
#define APB2AVSBUS_AVS_READBACK_REG_ADDR       0x80100004
#define APB2AVSBUS_AVS_CMD_REG_ADDR            0x80100000

#define APB2AVSBUS_AVS_CMD_CMD_GRP_MASK                               0x8000000
#define APB2AVSBUS_AVS_CMD_CMD_CODE_MASK                              0x7800000
#define APB2AVSBUS_AVS_CMD_RAIL_SEL_MASK                              0x780000
#define APB2AVSBUS_AVS_READBACK_CMD_DATA_MASK                         0xFFFF00
#define APB2AVSBUS_AVS_FIFOS_STATUS_CMD_FIFO_VACANT_SLOTS_MASK        0xF00
#define APB2AVSBUS_AVS_FIFOS_STATUS_READBACK_FIFO_OCCUPIED_SLOTS_MASK 0xF0000

#define APB2AVSBUS_AVS_CMD_CMD_DATA_SHIFT       3
#define APB2AVSBUS_AVS_READBACK_CMD_DATA_SHIFT  8
#define APB2AVSBUS_AVS_CMD_RAIL_SEL_SHIFT       19
#define APB2AVSBUS_AVS_CMD_CMD_CODE_SHIFT       23
#define APB2AVSBUS_AVS_CMD_CMD_GRP_SHIFT        27
#define APB2AVSBUS_AVS_CMD_R_OR_W_SHIFT         28
#define APB2AVSBUS_AVS_READBACK_SLAVE_ACK_SHIFT 30

#define NULL                                 ((void *)0)
#define GET_AVS_FIELD_SHIFT(REG_NAME, FIELD) APB2AVSBUS_AVS_##REG_NAME##_##FIELD##_SHIFT
#define GET_AVS_FIELD_MASK(REG_NAME, FIELD)  APB2AVSBUS_AVS_##REG_NAME##_##FIELD##_MASK
#define AVS_RD_CMD_DATA                      0xffff
#define AVS_FORCE_RESET_DATA                 0x0
#define AVS_RAIL_SEL_BROADCAST               0xf
#define AVS_ERR_RB_DATA                      0xffff
#define AVSCLK_FREQ_MHZ                      20

/* command code, command group macros */
/* 0: defined by AVS spec, 1: vendor specific  */
#define AVS_CMD_VOLTAGE                0x0, 0
#define AVS_CMD_VOUT_TRANS_RATE        0x1, 0
#define AVS_CMD_CURRENT_READ           0x2, 0
#define AVS_CMD_TEMP_READ              0x3, 0
#define AVS_CMD_FORCE_RESET            0x4, 0
#define AVS_CMD_POWER_MODE             0x5, 0
#define AVS_CMD_STATUS                 0xe, 0
#define AVS_CMD_VERSION_READ           0xf, 0
#define AVS_CMD_SYS_INPUT_CURRENT_READ 0x0, 1

typedef struct {
	uint32_t avs_clock_select: 2;
	uint32_t rsvd_0: 6;
	uint32_t stop_avs_clock_on_idle: 1;
	uint32_t force_slave_resync_operation: 1;
	uint32_t turn_off_all_premux_clocks: 1;
	uint32_t rsvd_1: 5;
	uint32_t clk_divider_value: 8;
	uint32_t clk_divider_duty_cycle_numerator: 8;
} APB2AVSBUS_AVS_CFG_1_reg_t;

typedef union {
	uint32_t val;
	APB2AVSBUS_AVS_CFG_1_reg_t f;
} APB2AVSBUS_AVS_CFG_1_reg_u;

#define APB2AVSBUS_AVS_CFG_1_REG_DEFAULT (0x800A0000)

typedef enum {
	AVSCommitWrite = 0,
	AVSHoldWrite = 1,
	AVSRead = 3,
} AVSReadWriteType;

static void WaitCmdFifoNotFull(void)
{
	uint32_t cmd_fifo_vacant_slots = 0;

	do {
		cmd_fifo_vacant_slots = ReadReg(APB2AVSBUS_AVS_FIFOS_STATUS_REG_ADDR) &
					GET_AVS_FIELD_MASK(FIFOS_STATUS, CMD_FIFO_VACANT_SLOTS);
	} while (cmd_fifo_vacant_slots == 0);
}

static void WaitRxFifoNotEmpty(void)
{
	uint32_t readback_fifo_occupied_slots = 0;

	do {
		readback_fifo_occupied_slots =
			ReadReg(APB2AVSBUS_AVS_FIFOS_STATUS_REG_ADDR) &
			GET_AVS_FIELD_MASK(FIFOS_STATUS, READBACK_FIFO_OCCUPIED_SLOTS);
	} while (readback_fifo_occupied_slots == 0);
}

/* Assume users do not program max_retries while reading from the RX FIFO. */
/* TODO: log the debug status in CSM. */
static AVSStatus ReadRxFifo(uint16_t *response)
{
	uint8_t max_retries = ReadReg(APB2AVSBUS_AVS_CFG_0_REG_ADDR);
	uint32_t readback_data = 0;
	uint8_t num_tries = 0;
	AVSStatus slave_ack = AVSOk;

	do {
		WaitRxFifoNotEmpty();
		readback_data = ReadReg(APB2AVSBUS_AVS_READBACK_REG_ADDR);
		slave_ack = readback_data >> GET_AVS_FIELD_SHIFT(READBACK, SLAVE_ACK);
		num_tries++;
	} while (slave_ack != AVSOk && num_tries <= max_retries);

	if (response != NULL) {
		if (slave_ack != AVSOk) {
			*response = AVS_ERR_RB_DATA;
		} else {
			*response = (readback_data & GET_AVS_FIELD_MASK(READBACK, CMD_DATA)) >>
				    GET_AVS_FIELD_SHIFT(READBACK, CMD_DATA);
		}
	}

	return slave_ack;
}

static void SendCmd(uint16_t cmd_data, uint8_t rail_sel, uint8_t cmd_code, uint8_t cmd_grp,
		    AVSReadWriteType r_or_w)
{
	WaitCmdFifoNotFull();
	uint32_t cmd_data_pos = cmd_data << GET_AVS_FIELD_SHIFT(CMD, CMD_DATA);
	uint32_t rail_sel_pos = (rail_sel << GET_AVS_FIELD_SHIFT(CMD, RAIL_SEL)) &
				GET_AVS_FIELD_MASK(CMD, RAIL_SEL);
	uint32_t cmd_code_pos = (cmd_code << GET_AVS_FIELD_SHIFT(CMD, CMD_CODE)) &
				GET_AVS_FIELD_MASK(CMD, CMD_CODE);
	uint32_t cmd_grp_pos =
		(cmd_grp << GET_AVS_FIELD_SHIFT(CMD, CMD_GRP)) & GET_AVS_FIELD_MASK(CMD, CMD_GRP);
	uint32_t r_or_w_pos = r_or_w << GET_AVS_FIELD_SHIFT(CMD, R_OR_W);

	WriteReg(APB2AVSBUS_AVS_CMD_REG_ADDR,
		 cmd_data_pos | rail_sel_pos | cmd_code_pos | cmd_grp_pos | r_or_w_pos);
}

/* Program CFG_0, CFG_1 registers and interrupt settings. */
/* Use default max_retries, resync_interval, clk_divide_value, and clk_divider_duty_cycle_numerator
 */
void AVSInit(void)
{
	APB2AVSBUS_AVS_CFG_1_reg_u avs_cfg_1;

	avs_cfg_1.val = APB2AVSBUS_AVS_CFG_1_REG_DEFAULT;
	/* gate all clocks entering AVS clock mux - do this before changeing the clock divider
	 * settings.
	 */
	avs_cfg_1.f.turn_off_all_premux_clocks = 1;
	WriteReg(APB2AVSBUS_AVS_CFG_1_REG_ADDR, avs_cfg_1.val);
	/* use divided version of APB clock as AVS clock, and set the divider value to get a clock
	 * of 20MHz.
	 */
	uint32_t apb_clk_freq = 0;

	clock_control_get_rate(pll_dev_1, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_APBCLK,
			       &apb_clk_freq);
	avs_cfg_1.f.clk_divider_value = DIV_ROUND_UP(apb_clk_freq, AVSCLK_FREQ_MHZ);
	avs_cfg_1.f.avs_clock_select = 1;
	WriteReg(APB2AVSBUS_AVS_CFG_1_REG_ADDR, avs_cfg_1.val);
	/* enable all clocks entering AVS clock mux. */
	avs_cfg_1.f.turn_off_all_premux_clocks = 0;
	WriteReg(APB2AVSBUS_AVS_CFG_1_REG_ADDR, avs_cfg_1.val);
	/* when AVS bus is idle, gate avs_clock from running. */
	avs_cfg_1.f.stop_avs_clock_on_idle = 1;
	WriteReg(APB2AVSBUS_AVS_CFG_1_REG_ADDR, avs_cfg_1.val);
	Wait(WAIT_1US);

	/* Enable all interrupts. */
	WriteReg(APB2AVSBUS_AVS_INTERRUPT_MASK_REG_ADDR, 0);
}

AVSStatus AVSReadVoltage(uint8_t rail_sel, uint16_t *voltage_in_mV)
{
	SendCmd(AVS_RD_CMD_DATA, rail_sel, AVS_CMD_VOLTAGE, AVSRead);
	return ReadRxFifo(voltage_in_mV);
}

AVSStatus AVSWriteVoltage(uint16_t voltage_in_mV, uint8_t rail_sel)
{
	SendCmd(voltage_in_mV, rail_sel, AVS_CMD_VOLTAGE, AVSCommitWrite);
	AVSStatus status = ReadRxFifo(NULL);

	/* 150us to cover voltage switch from 0.65V to 0.95V with 50us of margin */
	WaitUs(150);
	return status;
}

AVSStatus AVSReadVoutTransRate(uint8_t rail_sel, uint8_t *rise_rate, uint8_t *fall_rate)
{
	SendCmd(AVS_RD_CMD_DATA, rail_sel, AVS_CMD_VOUT_TRANS_RATE, AVSRead);
	uint16_t trans_rate;
	AVSStatus status = ReadRxFifo(&trans_rate);
	*rise_rate = trans_rate >> 8;
	*fall_rate = trans_rate & 0xff;
	return status;
}

AVSStatus AVSWriteVoutTransRate(uint8_t rise_rate, uint8_t fall_rate, uint8_t rail_sel)
{
	uint16_t trans_rate = (rise_rate << 8) | fall_rate;

	SendCmd(trans_rate, rail_sel, AVS_CMD_VOUT_TRANS_RATE, AVSCommitWrite);
	return ReadRxFifo(NULL);
}

/* Returns current in A */
AVSStatus AVSReadCurrent(uint8_t rail_sel, float *current_in_A)
{
	SendCmd(AVS_RD_CMD_DATA, rail_sel, AVS_CMD_CURRENT_READ, AVSRead);
	uint16_t current_in_10mA;
	AVSStatus status = ReadRxFifo(&current_in_10mA);
	*current_in_A = current_in_10mA * 0.01f;
	return status;
}

AVSStatus AVSReadTemp(uint8_t rail_sel, float *temp_in_C)
{
	SendCmd(AVS_RD_CMD_DATA, rail_sel, AVS_CMD_TEMP_READ, AVSRead);
	uint16_t temp; /* 1LSB = 0.1degC  */
	AVSStatus status = ReadRxFifo(&temp);
	*temp_in_C = temp * 0.1;
	return status;
}

AVSStatus AVSForceVoltageReset(uint8_t rail_sel)
{
	SendCmd(AVS_FORCE_RESET_DATA, rail_sel, AVS_CMD_FORCE_RESET, AVSCommitWrite);
	return ReadRxFifo(NULL);
}

/* This command is not supported by MAX20816, but will be ACKed. */
AVSStatus AVSReadPowerMode(uint8_t rail_sel, AVSPwrMode *power_mode)
{
	SendCmd(AVS_RD_CMD_DATA, rail_sel, AVS_CMD_POWER_MODE, AVSRead);
	return ReadRxFifo((uint16_t *)power_mode);
}

/* This command is not supported by MAX20816, but will be ACKed. */
AVSStatus AVSWritePowerMode(AVSPwrMode power_mode, uint8_t rail_sel)
{
	SendCmd(power_mode, rail_sel, AVS_CMD_POWER_MODE, AVSCommitWrite);
	return ReadRxFifo(NULL);
}

AVSStatus AVSReadStatus(uint8_t rail_sel, uint16_t *status)
{
	SendCmd(AVS_RD_CMD_DATA, rail_sel, AVS_CMD_STATUS, AVSRead);
	return ReadRxFifo(status);
}

AVSStatus AVSWriteStatus(uint16_t status, uint8_t rail_sel)
{
	SendCmd(status, rail_sel, AVS_CMD_STATUS, AVSCommitWrite);
	return ReadRxFifo(NULL);
}

/* For AVSBus version read, the rail_sel is broadcast. */
/* only the lower 4bits are valid and should be zero for PMBus 1.3. */
/* Any other PMBus versions are not supported by the AVS controller. */
AVSStatus AVSReadVersion(uint16_t *version)
{
	SendCmd(AVS_RD_CMD_DATA, AVS_RAIL_SEL_BROADCAST, AVS_CMD_VERSION_READ, AVSRead);
	return ReadRxFifo(version);
}

AVSStatus AVSReadSystemInputCurrent(uint16_t *response)
{
	uint8_t rail_sel = 0x0; /* Rail A and Rail B return the same data. */

	SendCmd(AVS_RD_CMD_DATA, rail_sel, AVS_CMD_SYS_INPUT_CURRENT_READ, AVSRead);
	return ReadRxFifo(response);
	/* TODO: need to figure the formula to calculate the system input current */
	/* System Input Current (read only) returns the ADC output of voltage at IINSEN pin. */
	/* The raw ADC data is decoded to determine the VIINSEN voltage: */
	/* VIINSEN (V) = [(ADC in decimal) x 1.1064+43] x 0.001173 – 0.05 */
	/* The actual input current depends on how the current signal is converted to a voltage at
	 * the IINSEN pin. In the case of the MAX20816 EV Kit,
	 */
	/* Input Current (A) = VIINSEN / (RSHUNT x CSA_gain) */
	/* where RSHUNT is the input current sense resistor, and CSA_gain is the gain of the current
	 * sense amplifier.
	 */
}

static int avs_init(void)
{
	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY) || !IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	/* Initiate AVS interface and switch vout control to AVSBus */
	AVSInit();
	SwitchVoutControl(AVSVoutCommand);

	return 0;
}
SYS_INIT_APP(avs_init);
