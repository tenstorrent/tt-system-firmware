/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef REGULATOR_H
#define REGULATOR_H

#include <stdint.h>
#include <zephyr/drivers/misc/bh_fwtable.h>

/* I2C slave addresses */
#define SERDES_VDDL_ADDR            0x30
#define SERDES_VDD_ADDR             0x31
#define SERDES_VDDH_ADDR            0x32
#define GDDR_VDDR_ADDR              0x33
#define GDDRIO_WEST_ADDR            0x36
#define GDDRIO_EAST_ADDR            0x37
#define CB_GDDR_VDDR_WEST_ADDR      0x54
#define CB_GDDR_VDDR_EAST_ADDR      0x55
#define SCRAPPY_GDDR_VDDR_WEST_ADDR 0x56
#define SCRAPPY_GDDR_VDDR_EAST_ADDR 0x57
#define P0V8_VCORE_ADDR             0x64
#define P0V8_VCOREM_ADDR            0x65

enum VoltageCmdSource {
	VoutCommand = 0,
	VoutMarginLow = 1,
	VoutMarginHigh = 2,
	AVSVoutCommand = 3,
};

uint32_t get_vcore(void);  /* returns voltage in mV. */
uint32_t get_vcorem(void); /* returns voltage in mV. */
void set_vcore(uint32_t voltage_in_mv);
void set_vcorem(uint32_t voltage_in_mv);
void set_gddr_vddr(PcbType board_type, uint32_t voltage_in_mv);
float GetVcoreCurrent(void);
float GetVcorePower(void);
void SwitchVoutControl(enum VoltageCmdSource source);
float GetGddrWestIoCurrent(void);
float GetGddrEastIoCurrent(void);
float GetGddrWestIoPower(void);
float GetGddrEastIoPower(void);
#endif
