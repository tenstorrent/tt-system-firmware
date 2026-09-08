/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SOC_TENSTORRENT_TT_GRENDEL_TT_GRENDEL_SMC_SOC_H_
#define SOC_TENSTORRENT_TT_GRENDEL_TT_GRENDEL_SMC_SOC_H_

#include <stdint.h>

#define SCRATCH_REG_BASE 0xC0010100

/* Convenience macro since we use scratch registers for debugging */
#define WRITE_SCRATCH(num, val) (*(((volatile uint32_t *)SCRATCH_REG_BASE) + ((num) * 2)) = (val))
#define READ_SCRATCH(num)       (*(((volatile uint32_t *)SCRATCH_REG_BASE) + ((num) * 2)))

/* Debug registers defined for this SOC */

/*
 * Written with the current program counter when
 * an unrecoverable system error occurs
 */
#define WRITE_CRASH_ADDR()                                                                         \
	uint32_t pc;                                                                               \
	__asm__ volatile("auipc %0, 0\n" : "=r"(pc) : :);                                          \
	WRITE_SCRATCH(0, pc);

static inline void test_pass(void)
{
	WRITE_SCRATCH(0, 0xacafaca1);
}

#endif /* SOC_TENSTORRENT_TT_GRENDEL_TT_GRENDEL_SMC_SOC_H_ */
