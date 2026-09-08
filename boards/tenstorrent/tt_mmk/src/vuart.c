/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <tenstorrent/uart_tt_virt.h>
#include <zephyr/kernel.h>

#include <soc.h>

void uart_tt_virt_init_callback(const struct device *dev, size_t inst)
{
	ARG_UNUSED(inst);
	WRITE_SCRATCH(2, (uint32_t)(uintptr_t)uart_tt_virt_get(dev));
}

volatile struct tt_vuart *uart_tt_virt_lookup_callback(size_t inst)
{
	ARG_UNUSED(inst);
	return (volatile struct tt_vuart *)(uintptr_t)READ_SCRATCH(2);
}
