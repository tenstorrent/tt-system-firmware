/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

/* Polled driver for the SMC apb2avsbus controller (AVSBus master). */

#include "avs_gr.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <platform.h>

#define AVS_PERIPHERAL_RESET_N BIT(8)

#define AVS_CMD_DATA     GENMASK(18, 3)
#define AVS_CMD_RAIL_SEL GENMASK(22, 19)
#define AVS_CMD_CODE     GENMASK(26, 23)
#define AVS_CMD_R_OR_W   GENMASK(29, 28)
#define AVS_R_OR_W_READ  3U
/* AVSBus read frames carry all-ones in the data field. */
#define AVS_READ_DATA    0xFFFFU

#define AVS_READBACK_CMD_DATA  GENMASK(23, 8)
#define AVS_READBACK_SLAVE_ACK GENMASK(31, 30)
#define AVS_SLAVE_ACK_OK       0U

#define AVS_NORMAL_STATUS_SLAVE_IN_RESYNC BIT(22)

#define AVS_FIFOS_CMD_VACANT         GENMASK(11, 8)
#define AVS_FIFOS_READBACK_OCCUPIED  GENMASK(19, 16)
/* Both FIFOs empty: 8 vacant slots each. */
#define AVS_FIFOS_STATUS_RESET_VALUE 0x08000800U

#define AVS_CFG_1_CLOCK_SELECT        GENMASK(1, 0)
#define AVS_CFG_1_STOP_ON_IDLE        BIT(8)
#define AVS_CFG_1_TURN_OFF_PREMUX_CLK BIT(10)
#define AVS_CFG_1_CLK_DIVIDER         GENMASK(23, 16)
#define AVS_CFG_1_DUTY_NUMERATOR      GENMASK(31, 24)
/* Divided APB clock; settings from the Sival SDK bring-up on Mimir/Keraunos SMC hardware. */
#define AVS_CLOCK_SELECT_DIVIDED_APB  1U
#define AVS_CLK_DIVIDER               0x10U
#define AVS_CLK_DUTY_NUMERATOR        0x80U

#define AVS_CMD_FIFO_MAX_POLLS 100000U
#define AVS_READBACK_MAX_POLLS 256U
#define AVS_RESYNC_MAX_POLLS   4096U

int gr_avs_init(void)
{
	uint32_t resets = sys_read32(SMC_CPU_RESET_UNIT_PERIPHERAL_RESETS_REG_ADDR);

	/* Other peripherals' reset bits share this register. */
	sys_write32(resets | AVS_PERIPHERAL_RESET_N, SMC_CPU_RESET_UNIT_PERIPHERAL_RESETS_REG_ADDR);

	if (sys_read32(SMC_CPU_APB2AVSBUS_AVS_FIFOS_STATUS_REG_ADDR) !=
	    AVS_FIFOS_STATUS_RESET_VALUE) {
		return -ENODEV;
	}

	/* The clock must be gated while the select and divider change. */
	uint32_t cfg_1 = AVS_CFG_1_TURN_OFF_PREMUX_CLK;

	sys_write32(cfg_1, SMC_CPU_APB2AVSBUS_AVS_CFG_1_REG_ADDR);
	cfg_1 |= FIELD_PREP(AVS_CFG_1_CLOCK_SELECT, AVS_CLOCK_SELECT_DIVIDED_APB) |
		 AVS_CFG_1_STOP_ON_IDLE | FIELD_PREP(AVS_CFG_1_CLK_DIVIDER, AVS_CLK_DIVIDER) |
		 FIELD_PREP(AVS_CFG_1_DUTY_NUMERATOR, AVS_CLK_DUTY_NUMERATOR);
	sys_write32(cfg_1, SMC_CPU_APB2AVSBUS_AVS_CFG_1_REG_ADDR);
	cfg_1 &= ~AVS_CFG_1_TURN_OFF_PREMUX_CLK;
	sys_write32(cfg_1, SMC_CPU_APB2AVSBUS_AVS_CFG_1_REG_ADDR);

	/*
	 * Restarting the clock starts a slave resync, but the resync bit only
	 * asserts a few cycles later: wait for it to assert, then to clear.
	 */
	uint32_t status = 0;

	for (uint32_t i = 0; i < AVS_RESYNC_MAX_POLLS; i++) {
		status = sys_read32(SMC_CPU_APB2AVSBUS_AVS_NORMAL_STATUS_REG_ADDR);
		if (status & AVS_NORMAL_STATUS_SLAVE_IN_RESYNC) {
			break;
		}
	}
	for (uint32_t i = 0; i < AVS_RESYNC_MAX_POLLS; i++) {
		status = sys_read32(SMC_CPU_APB2AVSBUS_AVS_NORMAL_STATUS_REG_ADDR);
		if (!(status & AVS_NORMAL_STATUS_SLAVE_IN_RESYNC)) {
			return 0;
		}
	}

	return -ETIMEDOUT;
}

int gr_avs_read(uint8_t rail, enum gr_avs_read_cmd cmd, uint16_t *data)
{
	uint32_t i;

	for (i = 0; i < AVS_CMD_FIFO_MAX_POLLS; i++) {
		if (FIELD_GET(AVS_FIFOS_CMD_VACANT,
			      sys_read32(SMC_CPU_APB2AVSBUS_AVS_FIFOS_STATUS_REG_ADDR))) {
			break;
		}
	}
	if (i == AVS_CMD_FIFO_MAX_POLLS) {
		return -ETIMEDOUT;
	}

	sys_write32(FIELD_PREP(AVS_CMD_DATA, AVS_READ_DATA) | FIELD_PREP(AVS_CMD_RAIL_SEL, rail) |
			    FIELD_PREP(AVS_CMD_CODE, cmd) |
			    FIELD_PREP(AVS_CMD_R_OR_W, AVS_R_OR_W_READ),
		    SMC_CPU_APB2AVSBUS_AVS_CMD_REG_ADDR);

	for (i = 0; i < AVS_READBACK_MAX_POLLS; i++) {
		if (FIELD_GET(AVS_FIFOS_READBACK_OCCUPIED,
			      sys_read32(SMC_CPU_APB2AVSBUS_AVS_FIFOS_STATUS_REG_ADDR))) {
			break;
		}
	}
	if (i == AVS_READBACK_MAX_POLLS) {
		return -ETIMEDOUT;
	}

	/* The controller has already retried a bad ack up to AVS_CFG_0.max_retries times. */
	uint32_t readback = sys_read32(SMC_CPU_APB2AVSBUS_AVS_READBACK_REG_ADDR);

	if (FIELD_GET(AVS_READBACK_SLAVE_ACK, readback) != AVS_SLAVE_ACK_OK) {
		return -EIO;
	}
	*data = FIELD_GET(AVS_READBACK_CMD_DATA, readback);

	return 0;
}
