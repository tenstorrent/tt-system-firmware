/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <tenstorrent/reg.h>

#define RESET_UNIT_REFCLK_CNT_LO_REG_ADDR 0x800300E0
#define RESET_UNIT_REFCLK_CNT_HI_REG_ADDR 0x800300E4

uint64_t TimerTimestamp(void)
{
	uint32_t reg_l = ReadReg(RESET_UNIT_REFCLK_CNT_LO_REG_ADDR);
	uint32_t reg_h = ReadReg(RESET_UNIT_REFCLK_CNT_HI_REG_ADDR);
	uint64_t timestamp = (uint64_t)reg_l | ((uint64_t)reg_h << 32);
	return timestamp;
}

void Wait(uint32_t cycles)
{
	uint64_t timestamp = TimerTimestamp();
	uint64_t time;

	do {
		time = TimerTimestamp();
	} while (time < (timestamp + (uint64_t)cycles));
}
