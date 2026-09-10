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

void write16_reg(uint64_t addr, uint16_t value);
uint16_t read16_reg(uint64_t addr);
void write32_reg(uint64_t addr, uint32_t value);
uint32_t read32_reg(uint64_t addr);
void write64_reg(uint64_t addr, uint64_t value);
uint64_t read64_reg(uint64_t addr);

#endif /* REGS_H */
