/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TENSTORRENT_REG_H_
#define TENSTORRENT_REG_H_

#include <stdint.h>

#if CONFIG_TT_FW_COMMON_EMUL
uint32_t ReadReg(uint32_t addr);
void WriteReg(uint32_t addr, uint32_t val);
#else
static inline uint32_t ReadReg(uint32_t addr)
{
	return *((uint32_t volatile *)addr);
}
static inline void WriteReg(uint32_t addr, uint32_t val)
{
	*((uint32_t volatile *)addr) = val;
}
#endif

#endif /* TENSTORRENT_REG_H_ */
