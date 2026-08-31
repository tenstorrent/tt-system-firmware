/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only MMIO capture. Included ahead of SiVal regs.h.
 */

#ifndef REGS_H
#define REGS_H

#include <stdint.h>

#include <zephyr/fff.h>

DECLARE_FAKE_VOID_FUNC(write16_reg, uint64_t, uint16_t);
DECLARE_FAKE_VALUE_FUNC(uint16_t, read16_reg, uint64_t);
DECLARE_FAKE_VOID_FUNC(write32_reg, uint64_t, uint32_t);
DECLARE_FAKE_VALUE_FUNC(uint32_t, read32_reg, uint64_t);
DECLARE_FAKE_VOID_FUNC(write64_reg, uint64_t, uint64_t);
DECLARE_FAKE_VALUE_FUNC(uint64_t, read64_reg, uint64_t);

#endif /* REGS_H */
