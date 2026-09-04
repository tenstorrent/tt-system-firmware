/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Keraunos SEP. With CONFIG_HARVARD the ARC linker takes ICCM as the romable
 * region and DCCM as the ramable one, so text lands where BL0 copies the
 * image and data/bss/stacks land in DCCM.
 */

#include <zephyr/devicetree.h>

#if DT_NODE_HAS_PROP(DT_INST(0, arc_iccm), reg) && \
	(DT_REG_SIZE(DT_INST(0, arc_iccm)) > 0)
#define ICCM_START DT_REG_ADDR(DT_INST(0, arc_iccm))
#define ICCM_SIZE  DT_REG_SIZE(DT_INST(0, arc_iccm))
#endif

#if DT_NODE_HAS_PROP(DT_INST(0, arc_dccm), reg) && \
	(DT_REG_SIZE(DT_INST(0, arc_dccm)) > 0)
#define DCCM_START DT_REG_ADDR(DT_INST(0, arc_dccm))
#define DCCM_SIZE  DT_REG_SIZE(DT_INST(0, arc_dccm))
#endif

#if DT_NODE_HAS_PROP(DT_CHOSEN(zephyr_sram), reg) && \
	(DT_REG_SIZE(DT_CHOSEN(zephyr_sram)) > 0)
#define SRAM_START DT_REG_ADDR(DT_CHOSEN(zephyr_sram))
#define SRAM_SIZE  DT_REG_SIZE(DT_CHOSEN(zephyr_sram))
#endif

#include <zephyr/arch/arc/v2/linker.ld>
