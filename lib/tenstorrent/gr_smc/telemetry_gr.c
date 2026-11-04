/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

/* Grendel telemetry: a sparse table holding only the tags Grendel implements. */

#include "avs_gr.h"
#include "telemetry.h"
#include "telemetry_platform.h"

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_io.h>

#include <platform.h>

BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_NODELABEL(telemetry_info), okay),
	     "Grendel telemetry requires telemetry_info status okay");

#define TELEMETRY_INFO_REG_ADDR (mem_addr_t) DT_REG_ADDR(DT_NODELABEL(telemetry_info))

/*
 * Word 0 is the chiplet id the SDK's smc_chiplet_id_get() reports.
 * Word 1 is the per-die instance id.
 */
#define FUSE_CHIPLET_ID_LOW_ADDR  (mem_addr_t) SMC_CPU_SMC_FUSE_MAP_CHIPLET_ID_REG_ADDR
#define FUSE_CHIPLET_ID_HIGH_ADDR (FUSE_CHIPLET_ID_LOW_ADDR + sizeof(uint32_t))

#define GR_SLOT_TAG_BOARD_ID_HIGH      0
#define GR_SLOT_TAG_BOARD_ID_LOW       1
#define GR_SLOT_TAG_CM_FW_VERSION      2
#define GR_SLOT_TAG_ASIC_ID_HIGH       3
#define GR_SLOT_TAG_ASIC_ID_LOW        4
#define GR_SLOT_TAG_UPDATE_TELEM_SPEED 5
#define GR_SLOT_TAG_TIMER_HEARTBEAT    6

/* Reported by the VR controller over AVSBus; 0 without CONFIG_TT_GR_SMC_AVS. */
#define GR_SLOT_TAG_VCORE            7
#define GR_SLOT_TAG_TDP              8
#define GR_SLOT_TAG_TDC              9
#define GR_SLOT_TAG_VREG_TEMPERATURE 10

#define GR_TAG_ENTRIES 11

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
		TAG_ENTRY(TAG_CM_FW_VERSION),
		TAG_ENTRY(TAG_ASIC_ID_HIGH),
		TAG_ENTRY(TAG_ASIC_ID_LOW),
		TAG_ENTRY(TAG_UPDATE_TELEM_SPEED),
		TAG_ENTRY(TAG_TIMER_HEARTBEAT),
		TAG_ENTRY(TAG_VCORE),
		TAG_ENTRY(TAG_TDP),
		TAG_ENTRY(TAG_TDC),
		TAG_ENTRY(TAG_VREG_TEMPERATURE)
	},
};
/* clang-format on */

#ifdef CONFIG_TT_GR_SMC_AVS
static bool avs_ready;
#endif

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
	telemetry_table.telemetry[GR_SLOT_TAG_ASIC_ID_HIGH] = sys_read32(FUSE_CHIPLET_ID_HIGH_ADDR);
	telemetry_table.telemetry[GR_SLOT_TAG_ASIC_ID_LOW] = sys_read32(FUSE_CHIPLET_ID_LOW_ADDR);
	telemetry_table.telemetry[GR_SLOT_TAG_UPDATE_TELEM_SPEED] = telemetry_update_interval_ms();

#ifdef CONFIG_TT_GR_SMC_AVS
	avs_ready = gr_avs_init() == 0;
#endif
}

#ifdef CONFIG_TT_GR_SMC_AVS
/* A tag keeps its previous value when its read fails. */
static void update_vreg(void)
{
	uint16_t mv, current_10ma, temp_deci_c;
	bool have_mv = gr_avs_read(GR_AVS_RAIL_VCORE, GR_AVS_READ_VOLTAGE, &mv) == 0;
	bool have_current = gr_avs_read(GR_AVS_RAIL_VCORE, GR_AVS_READ_CURRENT, &current_10ma) == 0;
	uint32_t *telemetry = telemetry_table.telemetry;

	if (have_mv) {
		telemetry[GR_SLOT_TAG_VCORE] = mv;
	}
	if (have_current) {
		telemetry[GR_SLOT_TAG_TDC] = current_10ma / 100U;
	}
	if (have_mv && have_current) {
		/* mV * 10 mA = 10 uW */
		telemetry[GR_SLOT_TAG_TDP] = (uint32_t)mv * current_10ma / 100000U;
	}
	if (gr_avs_read(GR_AVS_RAIL_VCORE, GR_AVS_READ_TEMPERATURE, &temp_deci_c) == 0) {
		telemetry[GR_SLOT_TAG_VREG_TEMPERATURE] = (int32_t)(int16_t)temp_deci_c / 10;
	}
}
#endif

void telemetry_platform_update(void)
{
	telemetry_table.telemetry[GR_SLOT_TAG_TIMER_HEARTBEAT]++;

#ifdef CONFIG_TT_GR_SMC_AVS
	if (avs_ready) {
		update_vreg();
	}
#endif
}

void telemetry_platform_publish(void)
{
	/* Host reads the table address here; the data array follows the tag table. */
	sys_write32((uint32_t)(uintptr_t)&telemetry_table, TELEMETRY_INFO_REG_ADDR);
}
