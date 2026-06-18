/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "noc.h"
#include "noc2axi.h"
#include "noc_init.h"

typedef struct {
	uint32_t passthrough_bits: 24;
	uint32_t lower_addr_bits: 8;
} NOC2AXITlb0RegT;

typedef union {
	uint32_t val;
	NOC2AXITlb0RegT f;
} NOC2AXITlb0RegU;

typedef struct {
	uint32_t middle_addr_bits: 32;
} NOC2AXITlb1RegT;

typedef union {
	uint32_t val;
	NOC2AXITlb1RegT f;
} NOC2AXITlb1RegU;

typedef struct {
	uint32_t x_end: 6;
	uint32_t y_end: 6;
	uint32_t x_start: 6;
	uint32_t y_start: 6;
	uint32_t multicast_en: 1;
	uint32_t ordering_mode: 2;
	uint32_t linked: 1;
	uint32_t sidebands_reserved: 2;
	uint32_t reserved_1: 2;
} NOC2AXITlb2RegT;

typedef union {
	uint32_t val;
	NOC2AXITlb2RegT f;
} NOC2AXITlb2RegU;

typedef struct {
	uint32_t stride_x: 4;
	uint32_t stride_y: 4;
	uint32_t quad_exclude_x: 6;
	uint32_t quad_exclude_y: 6;
	uint32_t quad_exclude_ctrl: 4;
	uint32_t num_destinations: 8;
} NOC2AXITlb3RegT;

typedef union {
	uint32_t val;
	NOC2AXITlb3RegT f;
} NOC2AXITlb3RegU;

#define NOC2AXI_NUM_TLB_PER_RING 16
#define RING0_TLB_REG_OFFSET     0x1000
#define AXI2NOC_RING_SEL_BIT     15

#ifdef CONFIG_TT_BH_ARC_EMUL
#define NIU0_A_REG_SPACE_SIZE 0x10000
/* Running with emulated ARC behavior. Fake out TLB register space */
static uint8_t fake_niu_reg_space[NIU0_A_REG_SPACE_SIZE];
#define NIU_0_A_REG_MAP_BASE_ADDR ((uintptr_t)fake_niu_reg_space)
#else
#define NIU_0_A_REG_MAP_BASE_ADDR 0x80050000
#endif

static inline uint32_t volatile *GetTlbRegStartAddr(const uint8_t ring)
{
	uint32_t volatile *tlb_addr =
		(uint32_t volatile *)((NIU_0_A_REG_MAP_BASE_ADDR + RING0_TLB_REG_OFFSET) |
				      ((uint32_t)ring << AXI2NOC_RING_SEL_BIT));
	return tlb_addr;
}

static inline void WriteTlbSetup(const uint8_t ring, const uint8_t tlb_num, NOC2AXITlb0RegU tlb0,
				 NOC2AXITlb1RegU tlb1, NOC2AXITlb2RegU tlb2, NOC2AXITlb3RegU tlb3)
{
	uint32_t volatile *noc2axi_tlb = GetTlbRegStartAddr(ring);

	noc2axi_tlb[tlb_num * 2] = tlb0.val;
	noc2axi_tlb[tlb_num * 2 + 1] = tlb1.val;
	noc2axi_tlb[tlb_num + NOC2AXI_NUM_TLB_PER_RING * 2] = tlb2.val;
	noc2axi_tlb[tlb_num + NOC2AXI_NUM_TLB_PER_RING * 3] = tlb3.val;
}

void NOC2AXITlbSetup(const uint8_t ring, const uint8_t tlb_num, const uint8_t x, const uint8_t y,
		     const uint64_t addr)
{
	NOC2AXITlb0RegU tlb0;

	tlb0.f.passthrough_bits = 0;
	tlb0.f.lower_addr_bits = addr >> 24;
	NOC2AXITlb1RegU tlb1;

	tlb1.f.middle_addr_bits = addr >> 32;
	NOC2AXITlb2RegU tlb2;

	tlb2.val = 0;
	tlb2.f.x_end = x;
	tlb2.f.y_end = y;
	tlb2.f.ordering_mode = kNoc2AxiOrderingStrict;
	NOC2AXITlb3RegU tlb3 = {.val = 0};

	WriteTlbSetup(ring, tlb_num, tlb0, tlb1, tlb2, tlb3);
}

void NOC2AXIMulticastTlbSetup(const uint8_t ring, const uint8_t tlb_num, const uint8_t x_start,
			      const uint8_t y_start, const uint8_t x_end, const uint8_t y_end,
			      const uint64_t addr, Noc2AxiOrdering ordering)
{

	NOC2AXITlb0RegU tlb0;

	tlb0.f.passthrough_bits = 0;
	tlb0.f.lower_addr_bits = addr >> 24;
	NOC2AXITlb1RegU tlb1;

	tlb1.f.middle_addr_bits = addr >> 32;
	NOC2AXITlb2RegU tlb2;

	tlb2.val = 0;
	tlb2.f.x_start = x_start;
	tlb2.f.y_start = y_start;
	tlb2.f.x_end = x_end;
	tlb2.f.y_end = y_end;
	tlb2.f.ordering_mode = ordering;
	tlb2.f.multicast_en = 1;
	NOC2AXITlb3RegU tlb3 = {.val = 0};

	WriteTlbSetup(ring, tlb_num, tlb0, tlb1, tlb2, tlb3);
}

/* Broadcast to all unharvested Tensix. Requires NocInit to be called first to set up broadcast
 * disables.
 */
/* Then we can just broadcast to the entire NOC, skipping ARC's own row to workaround this bug:
 */
/* https://yyz-gitlab.local.tenstorrent.com/tenstorrent/syseng/-/issues/3401#note_191646 */
void NOC2AXITensixBroadcastTlbSetup(const uint8_t ring, const uint8_t tlb_num, const uint64_t addr,
				    Noc2AxiOrdering ordering)
{
	/* Broadcast, but skip the row the ARC is on. */

	uint8_t x_start;
	uint8_t y_start;
	uint8_t x_end;
	uint8_t y_end;

	if (ring == 0) {
		/* Skip row 0. */
		x_start = 2;
		y_start = 2;
		x_end = 1;
		y_end = 11;
	} else if (IsNocTranslationEnabled()) {
		/* Skip row 0. Translation is on so multicast rects run backwards. */
		x_start = 1;
		y_start = 11;
		x_end = 2;
		y_end = 2;
	} else {
		/* Skip row 11. Translation is off so multicast rects run forwards. */
		x_start = 0;
		y_start = 0;
		x_end = 16;
		y_end = 10;
	}

	NOC2AXIMulticastTlbSetup(ring, tlb_num, x_start, y_start, x_end, y_end, addr, ordering);
}
