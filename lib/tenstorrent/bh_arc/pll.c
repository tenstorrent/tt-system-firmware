/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pll.h"
#include <tenstorrent/reg.h>
#include "timer.h"

#include <stdbool.h>

#include <tenstorrent/post_code.h>
#include <tenstorrent/sys_init_defines.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>

#define VCO_MIN_FREQ              1600
#define VCO_MAX_FREQ              5000
#define CLK_COUNTER_REFCLK_PERIOD 1000

#define PLL_0_CNTL_PLL_CNTL_0_REG_ADDR          0x80020100
#define PLL_0_CNTL_PLL_CNTL_1_REG_ADDR          0x80020104
#define PLL_0_CNTL_PLL_CNTL_2_REG_ADDR          0x80020108
#define PLL_0_CNTL_PLL_CNTL_3_REG_ADDR          0x8002010C
#define PLL_0_CNTL_PLL_CNTL_4_REG_ADDR          0x80020110
#define PLL_0_CNTL_PLL_CNTL_5_REG_ADDR          0x80020114
#define PLL_0_CNTL_PLL_CNTL_6_REG_ADDR          0x80020118
#define PLL_0_CNTL_USE_POSTDIV_REG_ADDR         0x8002011C
#define PLL_CNTL_WRAPPER_PLL_LOCK_REG_ADDR      0x80020040
#define PLL_CNTL_WRAPPER_REFCLK_PERIOD_REG_ADDR 0x8002002C
#define PLL_0_CNTL_CLK_COUNTER_EN_REG_ADDR      0x80020130

typedef struct {
	uint32_t reset: 1;
	uint32_t pd: 1;
	uint32_t reset_lock: 1;
	uint32_t pd_bgr: 1;
	uint32_t bypass: 1;
} PLL_CNTL_PLL_CNTL_0_reg_t;

typedef union {
	uint32_t val;
	PLL_CNTL_PLL_CNTL_0_reg_t f;
} PLL_CNTL_PLL_CNTL_0_reg_u;

#define PLL_CNTL_PLL_CNTL_0_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t refdiv: 8;
	uint32_t postdiv: 8;
	uint32_t fbdiv: 16;
} PLL_CNTL_PLL_CNTL_1_reg_t;

typedef union {
	uint32_t val;
	PLL_CNTL_PLL_CNTL_1_reg_t f;
} PLL_CNTL_PLL_CNTL_1_reg_u;

#define PLL_CNTL_PLL_CNTL_1_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t ctrl_bus1: 8;
	uint32_t ctrl_bus2: 8;
	uint32_t ctrl_bus3: 8;
	uint32_t ctrl_bus4: 8;
} PLL_CNTL_PLL_CNTL_2_reg_t;

typedef union {
	uint32_t val;
	PLL_CNTL_PLL_CNTL_2_reg_t f;
} PLL_CNTL_PLL_CNTL_2_reg_u;

#define PLL_CNTL_PLL_CNTL_2_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t ctrl_bus5: 8;
	uint32_t test_bus: 8;
	uint32_t lock_detect1: 16;
} PLL_CNTL_PLL_CNTL_3_reg_t;

typedef union {
	uint32_t val;
	PLL_CNTL_PLL_CNTL_3_reg_t f;
} PLL_CNTL_PLL_CNTL_3_reg_u;

#define PLL_CNTL_PLL_CNTL_3_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t lock_detect2: 16;
	uint32_t lock_detect3: 16;
} PLL_CNTL_PLL_CNTL_4_reg_t;

typedef union {
	uint32_t val;
	PLL_CNTL_PLL_CNTL_4_reg_t f;
} PLL_CNTL_PLL_CNTL_4_reg_u;

#define PLL_CNTL_PLL_CNTL_4_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t postdiv0: 8;
	uint32_t postdiv1: 8;
	uint32_t postdiv2: 8;
	uint32_t postdiv3: 8;
} PLL_CNTL_PLL_CNTL_5_reg_t;

typedef union {
	uint32_t val;
	PLL_CNTL_PLL_CNTL_5_reg_t f;
} PLL_CNTL_PLL_CNTL_5_reg_u;

#define PLL_CNTL_PLL_CNTL_5_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t pll_use_postdiv0: 1;
	uint32_t pll_use_postdiv1: 1;
	uint32_t pll_use_postdiv2: 1;
	uint32_t pll_use_postdiv3: 1;
	uint32_t pll_use_postdiv4: 1;
	uint32_t pll_use_postdiv5: 1;
	uint32_t pll_use_postdiv6: 1;
	uint32_t pll_use_postdiv7: 1;
} PLL_CNTL_USE_POSTDIV_reg_t;

typedef union {
	uint32_t val;
	PLL_CNTL_USE_POSTDIV_reg_t f;
} PLL_CNTL_USE_POSTDIV_reg_u;

#define PLL_CNTL_USE_POSTDIV_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t pll0_lock: 1;
	uint32_t pll1_lock: 1;
	uint32_t pll2_lock: 1;
	uint32_t pll3_lock: 1;
	uint32_t pll4_lock: 1;
} PLL_CNTL_WRAPPER_PLL_LOCK_reg_t;

typedef union {
	uint32_t val;
	PLL_CNTL_WRAPPER_PLL_LOCK_reg_t f;
} PLL_CNTL_WRAPPER_PLL_LOCK_reg_u;

#define PLL_CNTL_WRAPPER_PLL_LOCK_REG_DEFAULT (0x00000000)

typedef enum {
	PLL0 = 0,
	PLL1 = 1,
	PLL2 = 2,
	PLL3 = 3,
	PLL4 = 4,
	PLL_COUNT = 5,
} PLLNum;

typedef enum {
	PLLOk = 0,
	PLLTimeout = 1,
} PLLStatus;

typedef struct {
	PLL_CNTL_PLL_CNTL_1_reg_u pll_cntl_1;
	PLL_CNTL_PLL_CNTL_2_reg_u pll_cntl_2;
	PLL_CNTL_PLL_CNTL_3_reg_u pll_cntl_3;
	PLL_CNTL_PLL_CNTL_5_reg_u pll_cntl_5;
	PLL_CNTL_USE_POSTDIV_reg_u use_postdiv;
} PLLSettings;

static const PLLSettings kPLLInitialSettings[PLL_COUNT] = {
	/* PLL0 - AICLK */
	{.pll_cntl_1 = {.f.refdiv = 2,
			.f.postdiv = 0,
			.f.fbdiv = 128},      /* 3200 MHz. Use VCO >= 2650 MHz:
					       * https://tenstorrent.atlassian.net/browse/SYS-777
					       */
	 .pll_cntl_2 = {.f.ctrl_bus1 = 0x18}, /* FOUT4PHASEEN, FOUTPOSTDIVEN bits asserted */
	 .pll_cntl_3 = {.f.ctrl_bus5 = 1},
	 .pll_cntl_5 = {.f.postdiv0 = 3,  /* = AICLK - 800 MHz */
			.f.postdiv1 = 0,  /* Disabled */
			.f.postdiv2 = 0,  /* Disabled */
			.f.postdiv3 = 0}, /* Disabled */
	 .use_postdiv = {.f.pll_use_postdiv0 = 1,
			 .f.pll_use_postdiv1 = 1,
			 .f.pll_use_postdiv2 = 1,
			 .f.pll_use_postdiv3 = 1}},
	/* PLL1 - ARCCLK, AXICLK, APBCLK */
	{.pll_cntl_1 = {.f.refdiv = 2, .f.postdiv = 0, .f.fbdiv = 192}, /* 4800 MHz */
	 .pll_cntl_2 = {.f.ctrl_bus1 = 0x18}, /* FOUT4PHASEEN, FOUTPOSTDIVEN bits asserted */
	 .pll_cntl_3 = {.f.ctrl_bus5 = 1},
	 .pll_cntl_5 = {.f.postdiv0 = 5,  /* ARCCLK - 800 MHz */
			.f.postdiv1 = 4,  /* AXICLK - 960 MHz to saturate PCIE DMA BW:
					   * https://tenstorrent.atlassian.net/browse/SYS-737
					   */
			.f.postdiv2 = 23, /* APBCLK - 100 MHz */
			.f.postdiv3 = 0}, /* Disabled */
	 .use_postdiv = {.f.pll_use_postdiv0 = 1,
			 .f.pll_use_postdiv1 = 1,
			 .f.pll_use_postdiv2 = 1,
			 .f.pll_use_postdiv3 = 1}},
	/* PLL2 - MACCLK, SECCLK */
	{.pll_cntl_1 = {.f.refdiv = 2, .f.postdiv = 0, .f.fbdiv = 68}, /* 1700 MHz */
	 .pll_cntl_2 = {.f.ctrl_bus1 = 0x18}, /* FOUT4PHASEEN, FOUTPOSTDIVEN bits asserted */
	 .pll_cntl_3 = {.f.ctrl_bus5 = 1},
	 .pll_cntl_5 = {.f.postdiv0 = 1,  /* MACCLK - 850 MHz */
			.f.postdiv1 = 0,  /* SECCLK - Disabled */
			.f.postdiv2 = 0,  /* Disabled */
			.f.postdiv3 = 0}, /* Disabled */
	 .use_postdiv = {.f.pll_use_postdiv0 = 1,
			 .f.pll_use_postdiv1 = 1,
			 .f.pll_use_postdiv2 = 1,
			 .f.pll_use_postdiv3 = 1}},
	/* PLL3 - GDDRMEMCLK */
	{.pll_cntl_1 = {.f.refdiv = 2, .f.postdiv = 0, .f.fbdiv = 120}, /* 3000 MHz */
	 .pll_cntl_2 = {.f.ctrl_bus1 = 0x18}, /* FOUT4PHASEEN, FOUTPOSTDIVEN bits asserted */
	 .pll_cntl_3 = {.f.ctrl_bus5 = 1},
	 .pll_cntl_5 = {.f.postdiv0 = 3,  /* GDDRMEMCLK - 750 MHz */
			.f.postdiv1 = 0,  /* Disabled */
			.f.postdiv2 = 0,  /* Disabled */
			.f.postdiv3 = 0}, /* Disabled */
	 .use_postdiv = {.f.pll_use_postdiv0 = 1,
			 .f.pll_use_postdiv1 = 1,
			 .f.pll_use_postdiv2 = 1,
			 .f.pll_use_postdiv3 = 1}},
	/* PLL4 - L2CPUCLK0,1,2,3 */
	{.pll_cntl_1 = {.f.refdiv = 2, .f.postdiv = 0, .f.fbdiv = 64}, /* 1600 MHz */
	 .pll_cntl_2 = {.f.ctrl_bus1 = 0x18}, /* FOUT4PHASEEN, FOUTPOSTDIVEN bits asserted */
	 .pll_cntl_3 = {.f.ctrl_bus5 = 1},
	 .pll_cntl_5 = {.f.postdiv0 = 1,  /* L2CPUCLK0 - 800 MHz */
			.f.postdiv1 = 1,  /* L2CPUCLK1 - 800 MHz */
			.f.postdiv2 = 1,  /* L2CPUCLK2 - 800 MHz */
			.f.postdiv3 = 1}, /* L2CPUCLK3 - 800 MHz */
	 .use_postdiv = {.f.pll_use_postdiv0 = 1,
			 .f.pll_use_postdiv1 = 1,
			 .f.pll_use_postdiv2 = 1,
			 .f.pll_use_postdiv3 = 1}},
};

#define PLL_CNTL_REG_OFFSET 0x100
#define GET_PLL_CNTL_ADDR(ID, REG_NAME)                                                            \
	(PLL_0_CNTL_##REG_NAME##_REG_ADDR + PLL_CNTL_REG_OFFSET * ID)

static void ConfigPLLVco(PLLNum pll_num, const PLLSettings *pll_settings)
{
	/* refdiv, postdiv, fbdiv */
	WriteReg(GET_PLL_CNTL_ADDR(pll_num, PLL_CNTL_1), pll_settings->pll_cntl_1.val);
	/* FOUT4PHASEEN, FOUTPOSTDIVEN */
	WriteReg(GET_PLL_CNTL_ADDR(pll_num, PLL_CNTL_2), pll_settings->pll_cntl_2.val);
	/* Disable SSCG */
	WriteReg(GET_PLL_CNTL_ADDR(pll_num, PLL_CNTL_3), pll_settings->pll_cntl_3.val);
}

static void ConfigExtPostDivs(PLLNum pll_num, const PLLSettings *pll_settings)
{
	/* Disable postdivs before changing postdivs */
	WriteReg(GET_PLL_CNTL_ADDR(pll_num, USE_POSTDIV), 0x0);
	/* Set postdivs */
	WriteReg(GET_PLL_CNTL_ADDR(pll_num, PLL_CNTL_5), pll_settings->pll_cntl_5.val);
	/* Enable postdivs */
	WriteReg(GET_PLL_CNTL_ADDR(pll_num, USE_POSTDIV), pll_settings->use_postdiv.val);
}

/* assume PLL Lock never times out */
static void WaitPLLLock(PLLNum pll_num)
{
	uint64_t end_time = TimerTimestamp() + 400 * WAIT_1US;
	PLL_CNTL_WRAPPER_PLL_LOCK_reg_u pll_lock_reg;

	do {
		pll_lock_reg.val = ReadReg(PLL_CNTL_WRAPPER_PLL_LOCK_REG_ADDR);
		if (pll_lock_reg.val & (1 << pll_num)) {
			return;
		}
	} while (TimerTimestamp() < end_time);
}

void PLLAllBypass(void)
{
	PLL_CNTL_PLL_CNTL_0_reg_u pll_cntl_0;

	for (uint32_t i = 0; i < PLL_COUNT; i++) {
		/* Bypass PLL to refclk */
		pll_cntl_0.val = ReadReg(GET_PLL_CNTL_ADDR(i, PLL_CNTL_0));
		pll_cntl_0.f.bypass = 0;
		WriteReg(GET_PLL_CNTL_ADDR(i, PLL_CNTL_0), pll_cntl_0.val);
	}

	WaitUs(3);

	for (uint32_t i = 0; i < PLL_COUNT; i++) {
		/* Disable all external postdivs on all PLLs */
		WriteReg(GET_PLL_CNTL_ADDR(i, USE_POSTDIV), 0);
	}
}

/* Redo PLLInit, but for a single PLL with new settings */
void PLLUpdate(PLLNum pll, const PLLSettings *pll_settings)
{
	PLL_CNTL_PLL_CNTL_0_reg_u pll_cntl_0;

	pll_cntl_0.val = ReadReg(GET_PLL_CNTL_ADDR(pll, PLL_CNTL_0));
	/* Before turning off PLL, bypass PLL so glitch free mux has no chance to switch */
	pll_cntl_0.f.bypass = 0;
	WriteReg(GET_PLL_CNTL_ADDR(pll, PLL_CNTL_0), pll_cntl_0.val);

	WaitUs(3);

	/* power down PLL, disable PLL reset */
	pll_cntl_0.val = 0;
	WriteReg(GET_PLL_CNTL_ADDR(pll, PLL_CNTL_0), pll_cntl_0.val);

	ConfigPLLVco(pll, pll_settings);

	/* power sequence requires PLLEN get asserted 1us after all inputs are stable.  */
	/* wait 5x this time to be convervative */
	WaitUs(5);

	/* power up PLLs */
	pll_cntl_0.f.pd = 1;
	WriteReg(GET_PLL_CNTL_ADDR(pll, PLL_CNTL_0), pll_cntl_0.val);

	/* wait for PLL to lock */
	WaitPLLLock(pll);

	/* setup external postdivs */
	ConfigExtPostDivs(pll, pll_settings);

	WaitNs(300);

	/* disable PLL bypass */
	pll_cntl_0.f.bypass = 1;
	WriteReg(GET_PLL_CNTL_ADDR(pll, PLL_CNTL_0), pll_cntl_0.val);

	WaitNs(300);
}

static void enable_clk_counters(void)
{
	WriteReg(PLL_CNTL_WRAPPER_REFCLK_PERIOD_REG_ADDR, CLK_COUNTER_REFCLK_PERIOD);
	for (PLLNum i = 0; i < PLL_COUNT; i++) {
		WriteReg(GET_PLL_CNTL_ADDR(i, CLK_COUNTER_EN), 0xff);
	}
}

/* set AICLK to 800 MHz, AXICLK and ARCCLK to 475 MHz, APBCLK to 118.75 MHz */
int PLLInit(void)
{
	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEP4);

	if (!IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	/* Init clocks to faster (but safe) levels */

	PLL_CNTL_PLL_CNTL_0_reg_u pll_cntl_0;

	for (PLLNum i = 0; i < PLL_COUNT; i++) {
		pll_cntl_0.val = ReadReg(GET_PLL_CNTL_ADDR(i, PLL_CNTL_0));
		/* Before turning off PLL, bypass PLL so glitch free mux has no chance to switch */
		pll_cntl_0.f.bypass = 0;
		WriteReg(GET_PLL_CNTL_ADDR(i, PLL_CNTL_0), pll_cntl_0.val);
	}

	WaitUs(3);

	for (PLLNum i = 0; i < PLL_COUNT; i++) {
		/* power down PLL, disable PLL reset */
		pll_cntl_0.val = 0;
		WriteReg(GET_PLL_CNTL_ADDR(i, PLL_CNTL_0), pll_cntl_0.val);
	}

	for (PLLNum i = 0; i < PLL_COUNT; i++) {
		ConfigPLLVco(i, &kPLLInitialSettings[i]);
	}

	/* power sequence requires PLLEN get asserted 1us after all inputs are stable.  */
	/* wait 5x this time to be convervative */
	WaitUs(5);

	/* power up PLLs */
	pll_cntl_0.f.pd = 1;
	for (PLLNum i = 0; i < PLL_COUNT; i++) {
		WriteReg(GET_PLL_CNTL_ADDR(i, PLL_CNTL_0), pll_cntl_0.val);
	}

	/* wait for PLLs to lock */
	for (PLLNum i = 0; i < PLL_COUNT; i++) {
		WaitPLLLock(i);
	}

	/* setup external postdivs */
	for (PLLNum i = 0; i < PLL_COUNT; i++) {
		ConfigExtPostDivs(i, &kPLLInitialSettings[i]);
	}

	WaitNs(300);

	/* disable PLL bypass */
	pll_cntl_0.f.bypass = 1;
	for (PLLNum i = 0; i < PLL_COUNT; i++) {
		WriteReg(GET_PLL_CNTL_ADDR(i, PLL_CNTL_0), pll_cntl_0.val);
	}

	WaitNs(300);

	enable_clk_counters();

	return 0;
}
SYS_INIT_APP(PLLInit);

uint32_t GetExtPostdiv(uint8_t postdiv_index, PLL_CNTL_PLL_CNTL_5_reg_u pll_cntl_5,
		       PLL_CNTL_USE_POSTDIV_reg_u use_postdiv)
{
	uint32_t postdiv_value;
	bool postdiv_enabled;

	switch (postdiv_index) {
	case 0:
		postdiv_value = pll_cntl_5.f.postdiv0;
		postdiv_enabled = use_postdiv.f.pll_use_postdiv0;
		break;
	case 1:
		postdiv_value = pll_cntl_5.f.postdiv1;
		postdiv_enabled = use_postdiv.f.pll_use_postdiv1;
		break;
	case 2:
		postdiv_value = pll_cntl_5.f.postdiv2;
		postdiv_enabled = use_postdiv.f.pll_use_postdiv2;
		break;
	case 3:
		postdiv_value = pll_cntl_5.f.postdiv3;
		postdiv_enabled = use_postdiv.f.pll_use_postdiv3;
		break;
	default:
		__builtin_unreachable();
	}
	if (postdiv_enabled) {
		uint32_t eff_postdiv;

		if (postdiv_value == 0) {
			eff_postdiv = 0;
		} else if (postdiv_value <= 16) {
			eff_postdiv = postdiv_value + 1;
		} else {
			eff_postdiv = (postdiv_value + 1) * 2;
		}

		return eff_postdiv;
	} else {
		return 1;
	}
}

/* What we don't support: */
/* 1. PLL_CNTL_O.bypass */
/* 2. Internal bypass */
/* 3. Internal postdiv - PLL_CNTL_1.postdiv */
/* 4. Fractional feedback divider */
/* 5. Fine Divider */
uint32_t CalculateFreqFromPllRegs(PLL_CNTL_PLL_CNTL_1_reg_u pll_cntl_1,
				  PLL_CNTL_PLL_CNTL_5_reg_u pll_cntl_5,
				  PLL_CNTL_USE_POSTDIV_reg_u use_postdiv, uint8_t postdiv_index)
{
	uint32_t refdiv = pll_cntl_1.f.refdiv;
	uint32_t fbdiv = pll_cntl_1.f.fbdiv;
	uint32_t eff_postdiv = GetExtPostdiv(postdiv_index, pll_cntl_5, use_postdiv);

	if (eff_postdiv == 0) {
		/* Means clock is disabled */
		return 0;
	}
	return (REFCLK_F_MHZ * fbdiv) / (refdiv * eff_postdiv);
}

uint32_t CalculateFbdiv(uint32_t target_freq_mhz, PLL_CNTL_PLL_CNTL_1_reg_u pll_cntl_1,
			PLL_CNTL_PLL_CNTL_5_reg_u pll_cntl_5,
			PLL_CNTL_USE_POSTDIV_reg_u use_postdiv, uint8_t postdiv_index)
{
	uint32_t eff_postdiv = GetExtPostdiv(postdiv_index, pll_cntl_5, use_postdiv);

	if (eff_postdiv == 0) {
		/* Means clock is disabled */
		return 0;
	}
	return target_freq_mhz * pll_cntl_1.f.refdiv * eff_postdiv / REFCLK_F_MHZ;
}

uint32_t GetVcoFreq(PLL_CNTL_PLL_CNTL_1_reg_u pll_cntl_1)
{
	return (REFCLK_F_MHZ * pll_cntl_1.f.fbdiv) / pll_cntl_1.f.refdiv;
}

uint32_t GetFreqFromPll(PLLNum pll_num, uint8_t postdiv_index)
{
	PLL_CNTL_PLL_CNTL_1_reg_u pll_cntl_1;

	pll_cntl_1.val = ReadReg(GET_PLL_CNTL_ADDR(pll_num, PLL_CNTL_1));
	PLL_CNTL_PLL_CNTL_5_reg_u pll_cntl_5;

	pll_cntl_5.val = ReadReg(GET_PLL_CNTL_ADDR(pll_num, PLL_CNTL_5));
	PLL_CNTL_USE_POSTDIV_reg_u use_postdiv;

	use_postdiv.val = ReadReg(GET_PLL_CNTL_ADDR(pll_num, USE_POSTDIV));
	return CalculateFreqFromPllRegs(pll_cntl_1, pll_cntl_5, use_postdiv, postdiv_index);
}

uint32_t GetAICLK(void)
{
	return GetFreqFromPll(PLL0, 0);
}

uint32_t GetARCCLK(void)
{
	return GetFreqFromPll(PLL1, 0);
}

uint32_t GetAXICLK(void)
{
	return GetFreqFromPll(PLL1, 1);
}

uint32_t GetAPBCLK(void)
{
	return GetFreqFromPll(PLL1, 2);
}

uint32_t GetL2CPUCLK(uint8_t l2cpu_num)
{
	return GetFreqFromPll(PLL4, l2cpu_num);
}

/**
 * Attempt to set the requested GDDRMEMCLK frequency
 *
 * This function tries to find a valid set of PLL settings to hit the requested GDDRMEMCLK
 * frequency. It then updates the PLL if valid settings are found.
 *
 * @return 0 on success, -1 on failure
 */
int SetGddrMemClk(uint32_t gddr_mem_clk_mhz)
{
	PLLSettings pll_settings = {
		.pll_cntl_1 = {.f.refdiv = 2, .f.postdiv = 0}, /* 3000 MHz */
		.pll_cntl_2 = {.f.ctrl_bus1 = 0x18}, /* FOUT4PHASEEN, FOUTPOSTDIVEN bits asserted */
		.pll_cntl_3 = {.f.ctrl_bus5 = 1},
		.pll_cntl_5 = {.f.postdiv0 = 3,  /* GDDRMEMCLK */
			       .f.postdiv1 = 0,  /* Disabled */
			       .f.postdiv2 = 0,  /* Disabled */
			       .f.postdiv3 = 0}, /* Disabled */
		.use_postdiv = {.f.pll_use_postdiv0 = 1,
				.f.pll_use_postdiv1 = 1,
				.f.pll_use_postdiv2 = 1,
				.f.pll_use_postdiv3 = 1}};
	uint32_t fbdiv = CalculateFbdiv(gddr_mem_clk_mhz, pll_settings.pll_cntl_1,
					pll_settings.pll_cntl_5, pll_settings.use_postdiv, 0);
	if (fbdiv == 0) {
		return -1;
	}
	pll_settings.pll_cntl_1.f.fbdiv = fbdiv;
	uint32_t vco_freq = GetVcoFreq(pll_settings.pll_cntl_1);

	if (!IN_RANGE(vco_freq, VCO_MIN_FREQ, VCO_MAX_FREQ)) {
		return -1;
	}

	PLLUpdate(PLL3, &pll_settings);
	return 0;
}

/* Assume: refdiv = 2, internal post div = 0, external post div = 1 */
/* use fbdiv is enabled */
void SetAICLK(uint32_t aiclk_in_mhz)
{
	/* calculate target FBDIV and actual aiclk */
	uint32_t target_fbdiv =
		(aiclk_in_mhz * 4) / REFCLK_F_MHZ; /* refdiv is 2, external postdiv is 1 */
	/* uint32_t target_aiclk = (REFCLK_F_MHZ * target_fbdiv) / 4; */
	uint32_t target_postdiv = 1;

	/* get current fbdiv and postdiv */
	PLL_CNTL_PLL_CNTL_1_reg_u pll_cntl_1;

	pll_cntl_1.val = ReadReg(GET_PLL_CNTL_ADDR(PLL0, PLL_CNTL_1));
	PLL_CNTL_PLL_CNTL_5_reg_u pll_cntl_5;

	pll_cntl_5.val = ReadReg(GET_PLL_CNTL_ADDR(PLL0, PLL_CNTL_5));

	/* baby step fbdiv and post div */
	while (pll_cntl_1.f.fbdiv != target_fbdiv) {
		if (target_fbdiv > pll_cntl_1.f.fbdiv) {
			pll_cntl_1.f.fbdiv += 1;
		} else if (target_fbdiv < pll_cntl_1.f.fbdiv) {
			pll_cntl_1.f.fbdiv -= 1;
		}
		WriteReg(GET_PLL_CNTL_ADDR(PLL0, PLL_CNTL_1), pll_cntl_1.val);
		WaitNs(100); /* TODO: we need to characterize this timing */
	}

	while (pll_cntl_5.f.postdiv0 != target_postdiv) {
		if (target_postdiv > pll_cntl_5.f.postdiv0) {
			pll_cntl_5.f.postdiv0 += 1;
		} else if (target_postdiv < pll_cntl_5.f.postdiv0) {
			pll_cntl_5.f.postdiv0 -= 1;
		}
		WriteReg(GET_PLL_CNTL_ADDR(PLL0, PLL_CNTL_5), pll_cntl_5.val);
		WaitNs(100); /* TODO: we need to characterize this timing */
	}
}

/* immediately add 10 dividers to reduce clock freq by 10 */
void DropAICLK(void)
{
	PLL_CNTL_PLL_CNTL_5_reg_u pll_cntl_5;

	pll_cntl_5.val = ReadReg(GET_PLL_CNTL_ADDR(PLL0, PLL_CNTL_5));
	pll_cntl_5.f.postdiv0 += 10;
	WriteReg(GET_PLL_CNTL_ADDR(PLL0, PLL_CNTL_5), pll_cntl_5.val);
	/* TODO: set aiclk_arb_max to MIN_FREQ */
}
