/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <tenstorrent/reg.h>

typedef struct {
	uint32_t dr: 1;
	uint32_t oe: 1;
	uint32_t pe: 1;
	uint32_t fe: 1;
	uint32_t bi: 1;
	uint32_t thre: 1;
	uint32_t temt: 1;
	uint32_t rfe: 1;
	uint32_t addr_rcvd: 1;
	uint32_t rsvd_lsr_31to9: 23;
} UART_ADDRESS_BLOCK_LSR_reg_t;

typedef union {
	uint32_t val;
	UART_ADDRESS_BLOCK_LSR_reg_t f;
} UART_ADDRESS_BLOCK_LSR_reg_u;

#define UART_ADDRESS_BLOCK_LSR_REG_DEFAULT 0x00000060

typedef struct {
	uint32_t uart_enable: 5;
	uint32_t preset: 1;
	uint32_t sreset: 1;
	uint32_t rsvd_0: 1;
	uint32_t pclk_disable: 1;
	uint32_t sclk_disable: 1;
	uint32_t rsvd_1: 6;
	uint32_t tx_ack: 1;
	uint32_t rx_ack: 1;
} RESET_UNIT_UART_CNTL_reg_t;

typedef union {
	uint32_t val;
	RESET_UNIT_UART_CNTL_reg_t f;
} RESET_UNIT_UART_CNTL_reg_u;

#define RESET_UNIT_UART_CNTL_REG_DEFAULT 0x00000000

typedef struct {
	uint32_t dls: 2;
	uint32_t stop: 1;
	uint32_t pen: 1;
	uint32_t eps: 1;
	uint32_t sp: 1;
	uint32_t bc: 1;
	uint32_t dlab: 1;
	uint32_t rsvd_lcr_31to8: 24;
} UART_ADDRESS_BLOCK_LCR_reg_t;

typedef union {
	uint32_t val;
	UART_ADDRESS_BLOCK_LCR_reg_t f;
} UART_ADDRESS_BLOCK_LCR_reg_u;

#define UART_ADDRESS_BLOCK_LCR_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t iid: 4;
	uint32_t rsvd_iir_5to4: 2;
	uint32_t fifose: 2;
	uint32_t rsvd_iir_31to8: 24;
} UART_ADDRESS_BLOCK_IIR_reg_t;

typedef union {
	uint32_t val;
	UART_ADDRESS_BLOCK_IIR_reg_t f;
} UART_ADDRESS_BLOCK_IIR_reg_u;

#define UART_ADDRESS_BLOCK_IIR_REG_DEFAULT 0x00000001

#define UART_ADDRESS_BLOCK_LSR_REG_ADDR 0x80200014
#define UART_ADDRESS_BLOCK_RBR_REG_ADDR 0x80200000

#define RESET_UNIT_GPIO4_PAD_RXEN_CNTL_REG_ADDR  0x800305AC
#define RESET_UNIT_GPIO4_PAD_TRIEN_CNTL_REG_ADDR 0x800305A0
#define UART_ADDRESS_BLOCK_LCR_REG_ADDR          0x8020000C
#define UART_ADDRESS_BLOCK_IER_REG_ADDR          0x80200004
#define UART_ADDRESS_BLOCK_DLF_REG_ADDR          0x802000C0
#define UART_ADDRESS_BLOCK_IIR_REG_ADDR          0x80200008

#define GET_RESET_DW_ADDR(REG_NAME) (RESET_UNIT_##REG_NAME##_REG_OFFSET >> 2)

static void WaitTxFifoEmpty(void)
{
	UART_ADDRESS_BLOCK_LSR_reg_u uart_lsr;

	do {
		uart_lsr.val = ReadReg(UART_ADDRESS_BLOCK_LSR_REG_ADDR);
	} while (uart_lsr.f.thre == 0);
}

static void WaitDataReady(void)
{
	UART_ADDRESS_BLOCK_LSR_reg_u uart_lsr;

	do {
		uart_lsr.val = ReadReg(UART_ADDRESS_BLOCK_LSR_REG_ADDR);
	} while (uart_lsr.f.dr == 0);
}

/* num_frame is less than the TX FIFO size */
void UartTransmitFrames(uint32_t num_frame, uint8_t *data)
{
	WaitTxFifoEmpty();
	for (uint32_t id = 0; id < num_frame; id++) {
		WriteReg(UART_ADDRESS_BLOCK_RBR_REG_ADDR,
			 data[id]); /* RBR and THR share the same address */
	}
}

/* receive one frame from RBR */
uint8_t UartReceiveFrame(void)
{
	WaitDataReady();
	return ReadReg(UART_ADDRESS_BLOCK_RBR_REG_ADDR);
}

void UartInit(void)
{
	/* Set GPIO49 trien and rxen to high for receiving */
	uint32_t gpio4_pad_trien_cntl = ReadReg(RESET_UNIT_GPIO4_PAD_TRIEN_CNTL_REG_ADDR);
	uint32_t gpio4_pad_rxen_cntl = ReadReg(RESET_UNIT_GPIO4_PAD_RXEN_CNTL_REG_ADDR);

	WriteReg(RESET_UNIT_GPIO4_PAD_TRIEN_CNTL_REG_ADDR, gpio4_pad_trien_cntl | 2);
	WriteReg(RESET_UNIT_GPIO4_PAD_RXEN_CNTL_REG_ADDR, gpio4_pad_rxen_cntl | 2);

	/* enable UART */
	RESET_UNIT_UART_CNTL_reg_u uart_cntl;

	uart_cntl.val = RESET_UNIT_UART_CNTL_REG_DEFAULT;
	uart_cntl.f.uart_enable = 0x3; /* Take over GPIO UART pads: standard UART sin/sout pins */

	/* Follow programming flow in dw_apb_uart data book */
	/* Write 1 to DLAB to enable baud rate configuration */
	UART_ADDRESS_BLOCK_LCR_reg_u uart_lcr;

	uart_lcr.val = UART_ADDRESS_BLOCK_LCR_REG_DEFAULT;
	uart_lcr.f.dlab = 1;
	WriteReg(UART_ADDRESS_BLOCK_LCR_REG_ADDR, uart_lcr.val);

	/* Write to DLL, DLH to set up divisor for required baud rate (9600) */
	/* Refer to section 3.1 of the UART test plan */
	WriteReg(UART_ADDRESS_BLOCK_RBR_REG_ADDR, 0x45);
	WriteReg(UART_ADDRESS_BLOCK_IER_REG_ADDR,
		 0x1); /* IER and DLH share the same register address */
	WriteReg(UART_ADDRESS_BLOCK_DLF_REG_ADDR, 0x6);

	/* Write 0 to DLAB to access RBR, THR, and IER */
	uart_lcr.f.dlab = 0;
	uart_lcr.f.dls = 0x3; /* 8-bit data frame */
	WriteReg(UART_ADDRESS_BLOCK_LCR_REG_ADDR, uart_lcr.val);

	/* Enable TX and RX FIFO */
	UART_ADDRESS_BLOCK_IIR_reg_u uart_iir;

	uart_iir.val = UART_ADDRESS_BLOCK_IIR_REG_DEFAULT;
	uart_iir.f.fifose = 1;
	WriteReg(UART_ADDRESS_BLOCK_IIR_REG_ADDR, uart_iir.val);
}
