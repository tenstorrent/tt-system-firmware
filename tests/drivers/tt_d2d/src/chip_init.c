/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Mirrors the bring-up the emulation tests perform before touching a D2D tile:
 * lift_all_cold_resets() and disable_smc_{in,out}bound_filters() in chippy's
 * validation/jtag/ip_lib/cpp/mimir/src/mimir_ip.cpp. Those drive the chip from
 * the host; this is the same sequence issued from the SMC, so the driver can
 * be exercised by firmware rather than by a host harness.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "chip_init.h"

/* Setting a bit deasserts that subsystem's cold reset; the register is a
 * read-modify-write, so unrelated subsystems keep whatever state they had.
 */
#define RESET_UNIT_SS_COLD_RESET_N 0xC0002040U

#define COLD_RESET_SMN  BIT(1)
#define COLD_RESET_ITN  BIT(2)
#define COLD_RESET_D2D0 BIT(16)
#define COLD_RESET_D2D1 BIT(20)

/*
 * Two filters per direction, each three 64-bit registers: config, start, end.
 * Filter 0 admits non-secure traffic and filter 1 does not, which is the
 * arrangement the reference bring-up uses to leave the bus fully open.
 */
#define SMC_INBOUND_FILTER_BASE  0xC0015000U
#define SMC_OUTBOUND_FILTER_BASE 0xC0016000U

#define FILTER_STRIDE       0x20U
#define FILTER_START_OFFSET 0x08U
#define FILTER_END_OFFSET   0x10U

/* Covers the whole 44-bit chiplet-local address space. */
#define FILTER_END_ADDR_ALL 0xFFFFFFFFFFFULL

#define FILTER_CFG_READ_EN   BIT64(0)
#define FILTER_CFG_WRITE_EN  BIT64(1)
#define FILTER_CFG_ADDR_MODE BIT64(4)
#define FILTER_CFG_ALLOW_NS  BIT64(8)
#define FILTER_CFG_BURST     BIT64(24)

/* 7 selects the 128-bit data bus. */
#define FILTER_CFG_DATA_WIDTH_128 (7ULL << 12)

static void filter_open(uintptr_t filter_base, bool allow_non_secure)
{
	uint64_t config = FILTER_CFG_READ_EN | FILTER_CFG_WRITE_EN | FILTER_CFG_ADDR_MODE |
			  FILTER_CFG_DATA_WIDTH_128 | FILTER_CFG_BURST;

	if (allow_non_secure) {
		config |= FILTER_CFG_ALLOW_NS;
	}

	sys_write64(config, filter_base);
	sys_write64(0, filter_base + FILTER_START_OFFSET);
	sys_write64(FILTER_END_ADDR_ALL, filter_base + FILTER_END_OFFSET);
}

static void filters_open(uintptr_t base)
{
	filter_open(base, true);
	filter_open(base + FILTER_STRIDE, false);
}

void tt_d2d_test_chip_init(void)
{
	uint32_t cold_reset = sys_read32(RESET_UNIT_SS_COLD_RESET_N);

	cold_reset |= COLD_RESET_SMN | COLD_RESET_ITN | COLD_RESET_D2D0 | COLD_RESET_D2D1;
	sys_write32(cold_reset, RESET_UNIT_SS_COLD_RESET_N);

	filters_open(SMC_INBOUND_FILTER_BASE);
	filters_open(SMC_OUTBOUND_FILTER_BASE);
}
