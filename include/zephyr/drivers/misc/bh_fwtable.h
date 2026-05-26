/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BH_FWTABLE_H
#define BH_FWTABLE_H

#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FIXME: these proto files as well as all of the entities within them need to be namespaced
 * with a tt_bh_fwtable_ prefix (or something unique and similar).
 */
#include "fw_table.pb.h"
#include "flash_info.pb.h"
#include "read_only.pb.h"

const struct _FwTable *tt_bh_fwtable_get_fw_table(const struct device *dev);
const struct _FlashInfoTable *tt_bh_fwtable_get_flash_info_table(const struct device *dev);
const struct _ReadOnly *tt_bh_fwtable_get_read_only_table(const struct device *dev);

/* Board type values extracted from board_id */
#define BOARDTYPE_ORION_SLT 0x37
#define BOARDTYPE_P100A 0x43
#define BOARDTYPE_P150A 0x40
#define BOARDTYPE_P150  0x41
#define BOARDTYPE_P150C 0x42
#define BOARDTYPE_P300  0x44
#define BOARDTYPE_P300A 0x45
#define BOARDTYPE_P300C 0x46
#define BOARDTYPE_UBB   0x47

typedef enum {
	PcbTypeOrionSLT = 0,
	PcbTypeP100 = 1,
	PcbTypeP150 = 2,
	PcbTypeP300 = 3,
	PcbTypeUBB = 4,
	PcbTypeUnknown = 0xFF
} PcbType;

PcbType tt_bh_fwtable_get_pcb_type(const struct device *dev);
uint8_t tt_bh_fwtable_get_board_type(const struct device *dev);
bool tt_bh_fwtable_is_p300_left_chip(void);
uint32_t tt_bh_fwtable_get_asic_location(const struct device *dev);
void tt_bh_fwtable_apply_ccfgovr(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* BH_FWTABLE_H*/
