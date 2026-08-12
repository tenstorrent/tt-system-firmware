/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "debug_trace.h"
#include <tenstorrent/reg.h>

typedef struct {
	uint32_t interface_en: 1;
	uint32_t arcclk_disable: 1;
	uint32_t refclk_disable: 1;
} RESET_UNIT_CHIP_DEBUG_TRACE_IF_CNTL_reg_t;

typedef union {
	uint32_t val;
	RESET_UNIT_CHIP_DEBUG_TRACE_IF_CNTL_reg_t f;
} RESET_UNIT_CHIP_DEBUG_TRACE_IF_CNTL_reg_u;

#define RESET_UNIT_CHIP_DEBUG_TRACE_IF_CNTL_REG_DEFAULT    0x00000000
#define CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_IF_CNTL_REG_ADDR 0x80300004

typedef struct {
	uint32_t operation_mode: 1;
	uint32_t internal_fifo_flush: 1;
	uint32_t trace_buffer_flush: 1;
} CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_IF_CNTL_reg_t;

typedef union {
	uint32_t val;
	CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_IF_CNTL_reg_t f;
} CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_IF_CNTL_reg_u;

#define CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_IF_CNTL_REG_DEFAULT 0x00000000

typedef struct {
	uint32_t per_tick_increment: 26;
	uint32_t rsvd_0: 5;
	uint32_t clear: 1;
} CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_REFCLK_COUNTER_CNTL_reg_t;

typedef union {
	uint32_t val;
	CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_REFCLK_COUNTER_CNTL_reg_t f;
} CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_REFCLK_COUNTER_CNTL_reg_u;

#define CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_REFCLK_COUNTER_CNTL_REG_DEFAULT 0x00000000

typedef struct {
	uint32_t internal_fifo_overflow_clear: 1;
	uint32_t trace_buffer_overflow_clear: 1;
	uint32_t trace_buffer_almost_full_clear: 1;
	uint32_t internal_fifo_overflow_mask: 1;
	uint32_t trace_buffer_overflow_mask: 1;
	uint32_t trace_buffer_almost_full_mask: 1;
	uint32_t internal_fifo_overflow_status: 1;
	uint32_t trace_buffer_overflow_status: 1;
	uint32_t trace_buffer_almost_full_status: 1;
	uint32_t rsvd_0: 7;
	uint32_t trace_buffer_almost_full_cntl: 16;
} CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_BUFFER_INTR_CNTL_reg_t;

typedef union {
	uint32_t val;
	CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_BUFFER_INTR_CNTL_reg_t f;
} CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_BUFFER_INTR_CNTL_reg_u;

#define CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_BUFFER_INTR_CNTL_REG_DEFAULT 0x00010038

typedef struct {
	uint32_t enable: 1;
	uint32_t rsvd_0: 3;
	uint32_t client_id_size: 4;
} CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_CLIENT_FILTER_CNT_reg_t;

typedef union {
	uint32_t val;
	CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_CLIENT_FILTER_CNT_reg_t f;
} CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_CLIENT_FILTER_CNT_reg_u;

#define CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_CLIENT_FILTER_CNT_REG_DEFAULT 0x00000040

#define CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_BUFFER_DESTINATION_ADDR_REG_ADDR 0x80300014
#define RESET_UNIT_CHIP_DEBUG_TRACE_IF_CNTL_REG_ADDR                       0x80030C20
#define CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_BUFFER_DESTINATION_SIZE_REG_ADDR 0x80300018
#define CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_BUFFER_INTR_CNTL_REG_ADDR        0x8030001C
#define CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_REFCLK_COUNTER_CNTL_REG_ADDR     0x80300020
#define CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_CLIENT_FILTER_CNT_REG_ADDR       0x80300028

void DebugTraceInit(TraceBufferMode trace_buffer_mode, uint32_t trace_buffer_addr,
		    uint32_t trace_buffer_size)
{
	/* turn off interface and enable clock paths */
	RESET_UNIT_CHIP_DEBUG_TRACE_IF_CNTL_reg_u debug_trace_if_cntl;

	debug_trace_if_cntl.f.interface_en = 0;
	debug_trace_if_cntl.f.arcclk_disable = 0;
	debug_trace_if_cntl.f.refclk_disable = 0;
	WriteReg(RESET_UNIT_CHIP_DEBUG_TRACE_IF_CNTL_REG_ADDR, debug_trace_if_cntl.val);

	/* trace buffer mode, size, address, timestamp */
	CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_IF_CNTL_reg_u cntl;

	cntl.f.operation_mode = trace_buffer_mode;
	WriteReg(CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_IF_CNTL_REG_ADDR, cntl.val);
	WriteReg(CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_BUFFER_DESTINATION_ADDR_REG_ADDR,
		 trace_buffer_addr);
	WriteReg(CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_BUFFER_DESTINATION_SIZE_REG_ADDR,
		 trace_buffer_size);
	CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_REFCLK_COUNTER_CNTL_reg_u counter_cntl;

	counter_cntl.f.per_tick_increment = 0; /* 1 refclk per timestamp increment */
	WriteReg(CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_REFCLK_COUNTER_CNTL_REG_ADDR, counter_cntl.val);

	/* Interrupt control */
	CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_BUFFER_INTR_CNTL_reg_u interrupt_cntl;

	interrupt_cntl.val = CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_BUFFER_INTR_CNTL_REG_DEFAULT;
	interrupt_cntl.f.trace_buffer_almost_full_cntl = 1;
	interrupt_cntl.f.internal_fifo_overflow_mask = 0;
	interrupt_cntl.f.trace_buffer_overflow_mask = 0;
	interrupt_cntl.f.trace_buffer_almost_full_mask = 0;
	WriteReg(CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_BUFFER_INTR_CNTL_REG_ADDR, interrupt_cntl.val);

	/* disable client ID filtering */
	CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_CLIENT_FILTER_CNT_reg_u client_filtering_cntl;

	client_filtering_cntl.f.enable = 0;
	WriteReg(CHIP_DEBUG_TRACE_CHIP_DEBUG_TRACE_CLIENT_FILTER_CNT_REG_ADDR,
		 client_filtering_cntl.val);

	/* enable interface */
	debug_trace_if_cntl.f.interface_en = 1;
	WriteReg(RESET_UNIT_CHIP_DEBUG_TRACE_IF_CNTL_REG_ADDR, debug_trace_if_cntl.val);
}
