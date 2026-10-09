/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pcie_ltssm_log.h"
#include "reg.h"
#include "status_reg.h"

#include <zephyr/sys/barrier.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#define LTSSM_LOG_CAPACITY CONFIG_TT_BH_ARC_PCIE_LTSSM_LOG_ENTRIES

BUILD_ASSERT(LTSSM_LOG_CAPACITY >= 3, "LTSSM log needs room for the two newest entries");

/* In BSS so the linker places it in host-readable CSM */
static struct {
	struct ltssm_log_header header;
	struct ltssm_log_entry entries[LTSSM_LOG_CAPACITY];
} ltssm_log __aligned(32) __attribute__((section(".bss.pcie_ltssm_log")));

/* Each instance's two newest slots (-1 = none); a slot the other instance reused fails on INST */
static int32_t ltssm_log_newest[2] = {-1, -1};
static int32_t ltssm_log_prev[2] = {-1, -1};

void ltssm_log_reset(void)
{
	ltssm_log.header = (struct ltssm_log_header){
		.magic = LTSSM_LOG_MAGIC,
		.version = LTSSM_LOG_VERSION,
		.entry_size = sizeof(struct ltssm_log_entry),
		.capacity = LTSSM_LOG_CAPACITY,
		.count = 0,
		.dropped = 0,
		.tick_hz = LTSSM_LOG_TICK_HZ,
		.head = 0,
	};

	for (int i = 0; i < ARRAY_SIZE(ltssm_log_newest); i++) {
		ltssm_log_newest[i] = -1;
		ltssm_log_prev[i] = -1;
	}

	WriteReg(LTSSM_LOG_ADDR_REG_ADDR, (uint32_t)&ltssm_log);
	WriteReg(LTSSM_LOG_SIZE_REG_ADDR, sizeof(ltssm_log));
}

static inline bool ltssm_log_entry_is(int32_t idx, uint32_t info, uint32_t cdr)
{
	return (ltssm_log.entries[idx].info & LTSSM_INFO_ID_MASK) == info &&
	       ltssm_log.entries[idx].cdr == cdr;
}

/* Fold another observation of an already recorded state into its entry. */
static void ltssm_log_bump(int32_t idx, uint64_t timestamp)
{
	struct ltssm_log_entry *entry = &ltssm_log.entries[idx];
	uint32_t repeat = entry->info >> LTSSM_INFO_REPEAT_SHIFT;
	uint64_t delta = timestamp - entry->timestamp;

	if (repeat < LTSSM_INFO_REPEAT_MAX) {
		repeat++;
	}

	entry->last_delta = delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
	/* Span before count, so a host that sees the new count also sees its span */
	barrier_dmem_fence_full();
	entry->info = (entry->info & ~(LTSSM_INFO_REPEAT_MAX << LTSSM_INFO_REPEAT_SHIFT)) |
		      (repeat << LTSSM_INFO_REPEAT_SHIFT);
}

void ltssm_log_record(uint8_t pcie_inst, uint8_t state, bool link_up, bool rdlh_link_up,
		      uint32_t cdr, uint64_t timestamp)
{
	uint32_t info = (state << LTSSM_INFO_STATE_SHIFT) & LTSSM_INFO_STATE_MASK;

	if (link_up) {
		info |= LTSSM_INFO_LINK_UP;
	}
	if (rdlh_link_up) {
		info |= LTSSM_INFO_RDLH_LINK_UP;
	}
	if (pcie_inst != 0) {
		info |= LTSSM_INFO_INST;
	}

	int32_t *newest = &ltssm_log_newest[pcie_inst != 0];
	int32_t *prev = &ltssm_log_prev[pcie_inst != 0];

	if (*newest >= 0 && ltssm_log_entry_is(*newest, info, cdr)) {
		ltssm_log_bump(*newest, timestamp);
		return;
	}

	/* Back to the previous state: fold it there, so two-state churn costs two entries */
	if (*prev >= 0 && ltssm_log_entry_is(*prev, info, cdr)) {
		ltssm_log_bump(*prev, timestamp);

		int32_t swap = *newest;

		*newest = *prev;
		*prev = swap;
		return;
	}

	uint32_t idx = ltssm_log.header.head;

	ltssm_log.entries[idx] = (struct ltssm_log_entry){
		.timestamp = timestamp,
		.info = info,
		.last_delta = 0,
		.cdr = cdr,
	};

	/* Entry before head, so a concurrent host read never sees it half written */
	barrier_dmem_fence_full();
	ltssm_log.header.head = (idx + 1) % LTSSM_LOG_CAPACITY;

	if (ltssm_log.header.count < LTSSM_LOG_CAPACITY) {
		ltssm_log.header.count++;
	} else {
		ltssm_log.header.dropped++;
	}

	*prev = *newest;
	*newest = idx;
}
