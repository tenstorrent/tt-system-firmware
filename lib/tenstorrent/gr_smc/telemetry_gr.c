/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

/* Grendel telemetry: a sparse table holding only the tags Grendel implements. */

#include "telemetry.h"
#include "telemetry_platform.h"

#include <stdint.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_io.h>

BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_NODELABEL(telemetry_info), okay),
	     "Grendel telemetry requires telemetry_info status okay");

#define TELEMETRY_INFO_REG_ADDR (mem_addr_t) DT_REG_ADDR(DT_NODELABEL(telemetry_info))

#define GR_TAG_ENTRIES 3

#define GR_SLOT_TAG_BOARD_ID_HIGH 0
#define GR_SLOT_TAG_BOARD_ID_LOW  1
#define GR_SLOT_TAG_CM_FW_VERSION 2

struct telemetry_entry {
	uint16_t tag;
	uint16_t offset;
};

/* Host-visible layout, same as Blackhole but with GR_TAG_ENTRIES entries. */
struct telemetry_table {
	uint32_t version;
	uint32_t entry_count;
	struct telemetry_entry tag_table[GR_TAG_ENTRIES];
	uint32_t telemetry[GR_TAG_ENTRIES];
};

#define TAG_ENTRY(tag) [GR_SLOT_##tag] = {tag, GR_SLOT_##tag}
/* clang-format off */
static struct telemetry_table telemetry_table = {
	.tag_table = {
		TAG_ENTRY(TAG_BOARD_ID_HIGH),
		TAG_ENTRY(TAG_BOARD_ID_LOW),
		TAG_ENTRY(TAG_CM_FW_VERSION)
	},
};
/* clang-format on */

int telemetry_platform_slot(uint16_t tag)
{
	for (int i = 0; i < GR_TAG_ENTRIES; i++) {
		if (telemetry_table.tag_table[i].tag == tag) {
			return telemetry_table.tag_table[i].offset;
		}
	}

	return -1;
}

uint32_t *telemetry_platform_data(void)
{
	return telemetry_table.telemetry;
}

void telemetry_platform_write_static(uint32_t app_version)
{
	telemetry_table.version = TELEMETRY_VERSION;
	telemetry_table.entry_count = GR_TAG_ENTRIES;

	/* No Grendel board ID source yet. */
	telemetry_table.telemetry[GR_SLOT_TAG_BOARD_ID_HIGH] = 0;
	telemetry_table.telemetry[GR_SLOT_TAG_BOARD_ID_LOW] = 0;
	telemetry_table.telemetry[GR_SLOT_TAG_CM_FW_VERSION] = app_version;
}

void telemetry_platform_update(void)
{
}

void telemetry_platform_publish(void)
{
	/* Host reads the table address here; the data array follows the tag table. */
	sys_write32((uint32_t)(uintptr_t)&telemetry_table, TELEMETRY_INFO_REG_ADDR);
}
