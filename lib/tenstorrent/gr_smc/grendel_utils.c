/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr port of the Sival SDK's common/src/utils.c: the timing hooks
 * (read_cycle, wait_loop_itr) the prebuilt driver archives (gddr, eth)
 * reference but do not define.
 */

#include <stdint.h>

#include <zephyr/kernel.h>

uint64_t read_cycle(void);
void wait_loop_itr(uint32_t iterations);

/*
 * 64-bit cycle counter, via Zephyr's kernel cycle counter (portable across the
 * SMC; the SDK only uses this for timing/heartbeat logging).
 */
uint64_t read_cycle(void)
{
#ifdef CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER
	return k_cycle_get_64();
#else
	return (uint64_t)k_cycle_get_32();
#endif
}

/*
 * Spin for a fixed iteration count. Explicit volatile loop with a compiler
 * barrier so it is not optimized away; matches utils.h's "spin N iterations"
 * contract.
 */
void wait_loop_itr(uint32_t iterations)
{
	for (volatile uint32_t i = 0; i < iterations; i++) {
		__asm__ volatile("" ::: "memory");
	}
}
