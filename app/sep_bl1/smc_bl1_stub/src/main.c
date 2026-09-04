/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stand-in for SMC BL1 in the SEP BL1 handoff test. It is the TOC[0] image of
 * the staged BUN2, so reaching main() proves the whole chain ran: the SEP-side
 * driver staged the bundle, SMC BL0P5 got its validation ACK, copied this
 * image to load_addr, and mret'd to entry_point.
 *
 * The PASS code lands in SMC scratch[0].
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <soc.h>

int main(void)
{
	printk("SMC BL1 stub: reached via BL0P5 handoff\n");
	test_pass();

	/* BL1 owns the core from here on; returning would panic BL0P5's caller. */
	for (;;) {
		k_cpu_idle();
	}

	return 0;
}
