/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SERDES_ETH_H
#define SERDES_ETH_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "serdes_ss_regs.h"

/* LANE OFFSETS */
#define LANE_OFFSET     0x00010000
#define LANE_BROADCAST  0x00200000
#define LANE_RX_OFFSET  0x00000000
#define LANE_TX_OFFSET  0x00001000
#define LANE_ETH_OFFSET 0x00002000
#define LANE_DFX_OFFSET 0x00003000
#define LANE_MAX        7

/* REGISTERS OFFSETS */
#define CMN_OFFSET  0x01000000 /* 0x100_0000 PMA AlphaCore */
#define PCS_OFFSET  0x02000000 /* PCIE PIPE CORE */
#define CTRL_OFFSET 0x03000000 /* TensTorrent Registers control serdes */

#define MAX_SERDES_INSTANCES 6

#define SERDES_INST_BASE_ADDR(inst) (PCIE_PHY_SERDES0_BASE + ((inst) % 3 * 0x4000000))
#define SERDES_INST_SRAM_ADDR(inst)                                                                \
	(SERDES_INST_BASE_ADDR(inst) + PCIE_PHY_SERDES_SRAM_START_REG_ADDR)

int LoadSerdesEthFw(uint32_t serdes_inst, uint32_t ring, uint8_t *buf, size_t buf_size,
		    size_t spi_address, size_t image_size);

#endif
