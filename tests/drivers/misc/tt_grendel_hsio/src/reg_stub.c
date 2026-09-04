/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "regs.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define REG_MAP_CELLS 256

struct reg_cell {
	uint64_t addr;
	uint64_t val;
	uint8_t width;
	bool used;
};

static struct reg_cell cells[REG_MAP_CELLS];

static struct reg_cell *find_cell(uint64_t addr, uint8_t width, bool alloc)
{
	int i;
	int free_i = -1;

	for (i = 0; i < REG_MAP_CELLS; i++) {
		if (cells[i].used && cells[i].addr == addr && cells[i].width == width) {
			return &cells[i];
		}
		if (!cells[i].used && free_i < 0) {
			free_i = i;
		}
	}

	if (!alloc || free_i < 0) {
		if (alloc) {
			abort();
		}
		return NULL;
	}

	cells[free_i].addr = addr;
	cells[free_i].val = 0;
	cells[free_i].width = width;
	cells[free_i].used = true;
	return &cells[free_i];
}

void tt_grendel_reg_stub_reset(void)
{
	memset(cells, 0, sizeof(cells));
}

void write16_reg(uint64_t addr, uint16_t value)
{
	struct reg_cell *c = find_cell(addr, 2, true);

	if (c != NULL) {
		c->val = value;
	}
}

uint16_t read16_reg(uint64_t addr)
{
	struct reg_cell *c = find_cell(addr, 2, false);

	return c != NULL ? (uint16_t)c->val : 0;
}

void write32_reg(uint64_t addr, uint32_t value)
{
	struct reg_cell *c = find_cell(addr, 4, true);

	if (c != NULL) {
		c->val = value;
	}
}

uint32_t read32_reg(uint64_t addr)
{
	struct reg_cell *c = find_cell(addr, 4, false);

	return c != NULL ? (uint32_t)c->val : 0;
}

void write64_reg(uint64_t addr, uint64_t value)
{
	struct reg_cell *c = find_cell(addr, 8, true);

	if (c != NULL) {
		c->val = value;
	}
}

uint64_t read64_reg(uint64_t addr)
{
	struct reg_cell *c = find_cell(addr, 8, false);

	return c != NULL ? c->val : 0;
}
