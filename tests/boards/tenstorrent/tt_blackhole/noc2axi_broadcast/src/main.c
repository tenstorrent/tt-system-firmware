/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include "harvesting.h"
#include "noc.h"
#include "noc2axi.h"
#include "noc_init.h"

/* TLB 0: broadcast write. TLB 1: unicast readback. */
#define BCAST_TLB 0
#define READ_TLB  1
#define NOC_RING  0

/*
 * Tensix L1 address for the test pattern. Must not overlap firmware-used
 * locations: TRISC code starts at 0x6000, sync counter is at 0x110000.
 */
#define TEST_L1_ADDR 0x100ull
#define TEST_PATTERN 0xDEADBEEFu

/*
 * Verify that a single broadcast write via NOC2AXITensixBroadcastTlbSetup
 * reaches every unharvested Tensix. Requires NocInit (broadcast exclusion)
 * and CalculateHarvesting (tile_enable) to have run via SYS_INIT already.
 */
ZTEST(noc2axi_broadcast, test_broadcast_reaches_all_unharvested_tensixes)
{
	/* Set up multicast TLB and write the test pattern to all Tensix L1 */
	NOC2AXITensixBroadcastTlbSetup(0, BCAST_TLB, TEST_L1_ADDR, kNoc2AxiOrderingStrict);
	NOC2AXIWrite32(0, BCAST_TLB, TEST_L1_ADDR, TEST_PATTERN);
	NOC2AXITensixBroadcastTlbSetup(1, BCAST_TLB, TEST_L1_ADDR + 4, kNoc2AxiOrderingStrict);
	NOC2AXIWrite32(1, BCAST_TLB, TEST_L1_ADDR + 4, TEST_PATTERN + 1);

	/*
	 * Read back from each unharvested Tensix via unicast.
	 *
	 * TLBs are programmed with pre-translation (logical) NOC coordinates.
	 * With NOC translation enabled, Tensix logical X occupies slots 1..7
	 * and 10..16; good columns fill from the front in ascending physical
	 * NOC X order, so the first popcount(tensix_col_enabled) slots are the
	 * enabled tiles.  Tensix logical Y is 2..11 (identity-mapped).
	 */
	static const uint8_t kTensixLogicalX[] = {1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15, 16};
	int num_enabled = POPCOUNT(tile_enable.tensix_col_enabled);

	for (int xi = 0; xi < num_enabled; xi++) {
		uint8_t noc_x = kTensixLogicalX[xi];

		for (uint8_t noc_y = 2; noc_y <= 11; noc_y++) {
			NOC2AXITlbSetup(NOC_RING, READ_TLB, noc_x, noc_y, TEST_L1_ADDR);
			uint32_t val0 = NOC2AXIRead32(NOC_RING, READ_TLB, TEST_L1_ADDR);

			zassert_equal(
				val0, TEST_PATTERN,
				"Tensix xi=%d noc_y=%d NOC(%d,%d): got 0x%08x, expected 0x%08x", xi,
				noc_y, noc_x, noc_y, val0, TEST_PATTERN);

			uint32_t val1 = NOC2AXIRead32(NOC_RING, READ_TLB, TEST_L1_ADDR + 4);

			zassert_equal(
				val1, TEST_PATTERN + 1,
				"Tensix xi=%d noc_y=%d NOC(%d,%d): got 0x%08x, expected 0x%08x", xi,
				noc_y, noc_x, noc_y, val1, TEST_PATTERN + 1);
		}
	}
}

/*
 * The host (tt-kmd with power_policy enabled) may send a POWER_SETTING
 * message during boot that clock-gates the Tensix tiles when no client has
 * requested them. L1 accesses to a clock-gated tile never complete, so
 * make sure the tiles are powered before touching them.
 */
static void *noc2axi_broadcast_setup(void)
{
	set_tensix_enable(true);
	return NULL;
}

ZTEST_SUITE(noc2axi_broadcast, NULL, noc2axi_broadcast_setup, NULL, NULL, NULL);
