/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr port of the Sival SDK's common/src/utils.c: the timing hooks
 * (read_cycle, wait_loop_itr) the prebuilt driver archives (gddr, eth)
 * reference but do not define.
 */

#include <stdint.h>

#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gr_smc, CONFIG_LOG_DEFAULT_LEVEL);

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

FUNC_NORETURN void gr_smc_jump_to(uintptr_t entry_point)
{
	LOG_INF("Jumping to %p", (void *)entry_point);

	(void)irq_lock();

	__asm__ volatile(
		/* Order completed memory writes before the handoff. */
		"fence\n"
		/* Make the mission image visible to instruction fetch. */
		"fence.i\n"
		/* Disable all machine interrupt sources. */
		"csrw mie, zero\n"
		/* Clear pending interrupt bits that are software-writable. */
		"csrw mip, zero\n"
		/* Order the cleanup before entering the mission image. */
		"fence\n"
		/* Set the next entry address as the mret destination. */
		"csrw mepc, %0\n"
		/* Select M-mode with MIE and MPIE cleared. */
		"li   t0, 0x1800\n"
		/* Apply the clean machine-mode interrupt state. */
		"csrw mstatus, t0\n"
		/* Enter the mission image and restore MIE from the cleared MPIE. */
		"mret\n"
		:
		: "r"(entry_point)
		: "t0", "memory");
	CODE_UNREACHABLE;
}
