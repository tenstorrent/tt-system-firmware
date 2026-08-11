/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/sys/util.h>

__weak uint32_t ReadReg(uint32_t addr)
{
	ARG_UNUSED(addr);
	return 0U;
}

__weak void WriteReg(uint32_t addr, uint32_t val)
{
	ARG_UNUSED(addr);
	ARG_UNUSED(val);
}
