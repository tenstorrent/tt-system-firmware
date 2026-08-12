/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <tenstorrent/reg.h>

#define PMM_BLOCK_PMM_CTRL_REG_ADDR    0x80400004
#define PMM_BLOCK_PMM_MESSAGE_REG_ADDR 0x80400000

typedef struct {
	uint32_t enable: 1;
	uint32_t clear: 1;
} PMM_BLOCK_PMM_CTRL_reg_t;

typedef union {
	uint32_t val;
	PMM_BLOCK_PMM_CTRL_reg_t f;
} PMM_BLOCK_PMM_CTRL_reg_u;

#define PMM_BLOCK_PMM_CTRL_REG_DEFAULT (0x00000001)

typedef struct {
	uint32_t data: 8;
	uint32_t busy: 4;
	uint32_t flag0: 1;
	uint32_t flag1: 1;
	uint32_t flag2: 1;
	uint32_t flag3: 1;
	uint32_t node_type: 3;
	uint32_t rsvd_0: 1;
	uint32_t y: 6;
	uint32_t x: 6;
} PMM_BLOCK_PMM_MESSAGE_reg_t;

typedef union {
	uint32_t val;
	PMM_BLOCK_PMM_MESSAGE_reg_t f;
} PMM_BLOCK_PMM_MESSAGE_reg_u;

#define PMM_BLOCK_PMM_MESSAGE_REG_DEFAULT (0x00000000)

void MailboxWrite(uint8_t data, uint8_t busy, uint8_t flag0, uint8_t flag1, uint8_t flag2,
		  uint8_t flag3, uint8_t node_type, uint8_t y, uint8_t x)
{
	PMM_BLOCK_PMM_MESSAGE_reg_u pmm_message;

	pmm_message.f.data = data;
	pmm_message.f.busy = busy;
	pmm_message.f.flag0 = flag0;
	pmm_message.f.flag1 = flag1;
	pmm_message.f.flag2 = flag2;
	pmm_message.f.flag3 = flag3;
	pmm_message.f.node_type = node_type;
	pmm_message.f.y = y;
	pmm_message.f.x = x;
	WriteReg(PMM_BLOCK_PMM_MESSAGE_REG_ADDR, pmm_message.val);
}

void ClearPMMStatus(void)
{
	PMM_BLOCK_PMM_CTRL_reg_u pmm_ctrl;

	pmm_ctrl.val = ReadReg(PMM_BLOCK_PMM_CTRL_REG_ADDR);
	pmm_ctrl.f.clear = 1;
	WriteReg(PMM_BLOCK_PMM_CTRL_REG_ADDR, pmm_ctrl.val);
}

void EnablePMM(void)
{
	PMM_BLOCK_PMM_CTRL_reg_u pmm_ctrl;

	pmm_ctrl.f.enable = 1;
	pmm_ctrl.f.clear = 0;
	WriteReg(PMM_BLOCK_PMM_CTRL_REG_ADDR, pmm_ctrl.val);
}

void DisablePMM(void)
{
	PMM_BLOCK_PMM_CTRL_reg_u pmm_ctrl;

	pmm_ctrl.f.enable = 0;
	pmm_ctrl.f.clear = 0;
	WriteReg(PMM_BLOCK_PMM_CTRL_REG_ADDR, pmm_ctrl.val);
}
