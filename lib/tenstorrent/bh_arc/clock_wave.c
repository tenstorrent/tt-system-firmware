/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "clock_wave.h"

#include <zephyr/kernel.h>
#include <tenstorrent/smc_msg.h>
#include <tenstorrent/msgqueue.h>

#include "timer.h"
#include <tenstorrent/reg.h>

#define PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_REG_ADDR 0x80020038

typedef struct {
	uint32_t aiclk_zsk_enb: 1;
	uint32_t aiclk_mesh_enb: 1;
} PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_reg_t;

typedef union {
	uint32_t val;
	PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_reg_t f;
} PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_reg_u;

#define PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_REG_DEFAULT 0x00000001

static void switch_clk_scheme(enum tt_clk_scheme clk_scheme)
{
	PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_reg_u clock_wave_cntl;

	clock_wave_cntl.val = 0;
	WriteReg(PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_REG_ADDR, clock_wave_cntl.val);
	Wait(10); /* both enables are off for 10 refclk cycles */
	if (clk_scheme == TT_CLK_SCHEME_CLOCK_WAVE) {
		clock_wave_cntl.f.aiclk_mesh_enb = 1;
		clock_wave_cntl.f.aiclk_zsk_enb = 0;
	} else {
		clock_wave_cntl.f.aiclk_mesh_enb = 0;
		clock_wave_cntl.f.aiclk_zsk_enb = 1;
	}
	WriteReg(PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_REG_ADDR, clock_wave_cntl.val);
	Wait(10); /* wait for 10 refclk cycles for aiclk to stablize */
}

/**
 * @brief Handler for TT_SMC_MSG_SWITCH_CLK_SCHEME messages
 *
 * @details Switches the clock scheme configuration. This affects the AI clock
 *          distribution and timing.
 *
 * @param request Pointer to the host request message, use request->switch_clk_scheme for structured
 *                access
 * @param response Pointer to the response message to be sent back to host
 *
 * @return 0 on success
 * @return non-zero on error
 *
 * @see switch_clk_scheme_rqst
 */
static uint8_t switch_clk_scheme_handler(const union request *request, struct response *response)
{
	uint32_t clk_scheme = request->switch_clk_scheme.scheme;

	switch_clk_scheme(clk_scheme);
	return 0;
}
REGISTER_MESSAGE(TT_SMC_MSG_SWITCH_CLK_SCHEME, switch_clk_scheme_handler);
