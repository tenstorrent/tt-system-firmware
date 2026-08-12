/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NOC2AXI_H
#define NOC2AXI_H

#include <stdint.h>
#include <zephyr/sys/util.h>
#include <tenstorrent/reg.h>

#define ARC_NOC0_BASE_ADDR 0xC0000000
#define ARC_NOC1_BASE_ADDR 0xE0000000

#define NOC_TLB_LOG_SIZE         24
#define NOC_TLB_WINDOW_ADDR_MASK ((1 << NOC_TLB_LOG_SIZE) - 1)

typedef enum {
	kNoc2AxiOrderingRelaxed = 0,
	kNoc2AxiOrderingStrict = 1,
	kNoc2AxiOrderingPosted = 2,
	kNoc2AxiOrderingPostedStrict = 3,
} Noc2AxiOrdering;

void NOC2AXITlbSetup(const uint8_t ring, const uint8_t tlb_num, const uint8_t x, const uint8_t y,
		     const uint64_t addr);
void NOC2AXIMulticastTlbSetup(const uint8_t ring, const uint8_t tlb_num, const uint8_t x_start,
			      const uint8_t y_start, const uint8_t x_end, const uint8_t y_end,
			      const uint64_t addr, Noc2AxiOrdering ordering);
void NOC2AXITensixBroadcastTlbSetup(const uint8_t ring, const uint8_t tlb_num, const uint64_t addr,
				    Noc2AxiOrdering ordering);

static inline void volatile *GetTlbWindowAddr(const uint8_t noc_id, const uint8_t tlb_entry,
					      const uint64_t addr)
{
	uint32_t noc_base_addr = (noc_id == 0) ? ARC_NOC0_BASE_ADDR : ARC_NOC1_BASE_ADDR;
	uint32_t volatile *_addr =
		(void volatile *)(noc_base_addr + (tlb_entry << NOC_TLB_LOG_SIZE) +
				  ((intptr_t)addr & NOC_TLB_WINDOW_ADDR_MASK));
	return _addr;
}

static inline void NOC2AXIWrite32(const uint8_t noc_id, const uint8_t tlb_entry,
				  const uint64_t addr, const uint32_t data)
{
	WriteReg((uint32_t)GetTlbWindowAddr(noc_id, tlb_entry, addr), data);
}

static inline void NOC2AXIWrite8(const uint8_t noc_id, const uint8_t tlb_entry, const uint64_t addr,
				 const uint8_t data)
{
	uint8_t volatile *_addr = GetTlbWindowAddr(noc_id, tlb_entry, addr);
	*_addr = data;
}

static inline uint32_t NOC2AXIRead32(const uint8_t noc_id, const uint8_t tlb_entry,
				     const uint64_t addr)
{
	return ReadReg((uint32_t)GetTlbWindowAddr(noc_id, tlb_entry, addr));
}

#endif
