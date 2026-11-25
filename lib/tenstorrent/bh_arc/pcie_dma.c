/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <tenstorrent/smc_msg.h>
#include <tenstorrent/msgqueue.h>

#include "util.h"
#include "pcie.h"

#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_STATUS_OFF_WRCH_0_REG_ADDR 0x00380080
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_INT_SETUP_OFF_WRCH_0_REG_ADDR        \
	0x00380088
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_MSI_STOP_LOW_OFF_WRCH_0_REG_ADDR     \
	0x00380090
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_MSI_STOP_HIGH_OFF_WRCH_0_REG_ADDR    \
	0x00380094
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_MSI_ABORT_LOW_OFF_WRCH_0_REG_ADDR    \
	0x003800A0
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_MSI_ABORT_HIGH_OFF_WRCH_0_REG_ADDR   \
	0x003800A4
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_MSI_MSGD_OFF_WRCH_0_REG_ADDR         \
	0x003800A8
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_EN_OFF_WRCH_0_REG_ADDR      0x00380000
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_SAR_LOW_OFF_WRCH_0_REG_ADDR 0x00380020
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_SAR_HIGH_OFF_WRCH_0_REG_ADDR         \
	0x00380024
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_DAR_LOW_OFF_WRCH_0_REG_ADDR 0x00380028
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_DAR_HIGH_OFF_WRCH_0_REG_ADDR         \
	0x0038002C
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_XFERSIZE_OFF_WRCH_0_REG_ADDR         \
	0x0038001C
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_DOORBELL_OFF_WRCH_0_REG_ADDR         \
	0x00380004
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_STATUS_OFF_RDCH_0_REG_ADDR 0x00380180
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_INT_SETUP_OFF_RDCH_0_REG_ADDR        \
	0x00380188
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_MSI_STOP_LOW_OFF_RDCH_0_REG_ADDR     \
	0x00380190
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_MSI_STOP_HIGH_OFF_RDCH_0_REG_ADDR    \
	0x00380194
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_MSI_ABORT_LOW_OFF_RDCH_0_REG_ADDR    \
	0x003801A0
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_MSI_ABORT_HIGH_OFF_RDCH_0_REG_ADDR   \
	0x003801A4
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_MSI_MSGD_OFF_RDCH_0_REG_ADDR         \
	0x003801A8
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_EN_OFF_RDCH_0_REG_ADDR      0x00380100
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_DAR_LOW_OFF_RDCH_0_REG_ADDR 0x00380128
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_DAR_HIGH_OFF_RDCH_0_REG_ADDR         \
	0x0038012C
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_SAR_LOW_OFF_RDCH_0_REG_ADDR 0x00380120
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_SAR_HIGH_OFF_RDCH_0_REG_ADDR         \
	0x00380124
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_DOORBELL_OFF_RDCH_0_REG_ADDR         \
	0x00380104
#define PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_XFERSIZE_OFF_RDCH_0_REG_ADDR         \
	0x0038011C

#define HDMA_REG_ADDR(reg) (PCIE_DBI_USP_A_BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_##reg##_REG_ADDR)

typedef struct {
	uint32_t stop_mask: 1;
	uint32_t watermark_mask: 1;
	uint32_t abort_mask: 1;
	uint32_t rsie: 1;
	uint32_t lsie: 1;
	uint32_t raie: 1;
	uint32_t laie: 1;
	uint32_t reserved_7_31: 25;
} BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_INT_SETUP_OFF_WRCH_0_reg_t;

typedef union {
	uint32_t val;
	BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_INT_SETUP_OFF_WRCH_0_reg_t f;
} BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_INT_SETUP_OFF_WRCH_0_reg_u;

#define BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_INT_SETUP_OFF_WRCH_0_REG_DEFAULT (0x00000007)

typedef struct {
	uint32_t stop_mask: 1;
	uint32_t watermark_mask: 1;
	uint32_t abort_mask: 1;
	uint32_t rsie: 1;
	uint32_t lsie: 1;
	uint32_t raie: 1;
	uint32_t laie: 1;
	uint32_t reserved_7_31: 25;
} BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_INT_SETUP_OFF_RDCH_0_reg_t;

typedef union {
	uint32_t val;
	BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_INT_SETUP_OFF_RDCH_0_reg_t f;
} BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_INT_SETUP_OFF_RDCH_0_reg_u;

#define BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_INT_SETUP_OFF_RDCH_0_REG_DEFAULT (0x00000007)

typedef enum {
	DMARunning = 1,
	DMAAborted = 2,
	DMAStopped = 3
} DMAStatus;

/* write transfer from the prespective of the chip. i.e., from chip to host */
bool PcieDmaWriteTransfer(uint64_t chip_addr, uint64_t host_addr, uint32_t transfer_size_bytes,
			  uint64_t msi_completion_addr, uint8_t completion_data)
{
	/* reject dma request if there is a pending transaction */
	uint32_t status = ReadDbiReg(HDMA_REG_ADDR(STATUS_OFF_WRCH_0));

	if (status == DMARunning) {
		return false;
	}

	/* Setup completion interrupt */
	BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_INT_SETUP_OFF_WRCH_0_reg_u int_setup;

	int_setup.val = 0;
	int_setup.f.rsie = 1;
	int_setup.f.raie = 1;
	WriteDbiReg(HDMA_REG_ADDR(INT_SETUP_OFF_WRCH_0), int_setup.val);
	WriteDbiReg(HDMA_REG_ADDR(MSI_STOP_LOW_OFF_WRCH_0), low32(msi_completion_addr));
	WriteDbiReg(HDMA_REG_ADDR(MSI_STOP_HIGH_OFF_WRCH_0), high32(msi_completion_addr));
	WriteDbiReg(HDMA_REG_ADDR(MSI_ABORT_LOW_OFF_WRCH_0),
		    low32(msi_completion_addr + sizeof(uint32_t)));
	WriteDbiReg(HDMA_REG_ADDR(MSI_ABORT_HIGH_OFF_WRCH_0),
		    high32(msi_completion_addr + sizeof(uint32_t)));
	WriteDbiReg(HDMA_REG_ADDR(MSI_MSGD_OFF_WRCH_0), completion_data);

	/* Enable write channel 0 */
	WriteDbiReg(HDMA_REG_ADDR(EN_OFF_WRCH_0), 0x1);

	WriteDbiReg(HDMA_REG_ADDR(SAR_LOW_OFF_WRCH_0), low32(chip_addr));
	WriteDbiReg(HDMA_REG_ADDR(SAR_HIGH_OFF_WRCH_0), high32(chip_addr));
	WriteDbiReg(HDMA_REG_ADDR(DAR_LOW_OFF_WRCH_0), low32(host_addr));
	WriteDbiReg(HDMA_REG_ADDR(DAR_HIGH_OFF_WRCH_0), high32(host_addr));
	WriteDbiReg(HDMA_REG_ADDR(XFERSIZE_OFF_WRCH_0), transfer_size_bytes);
	WriteDbiReg(HDMA_REG_ADDR(DOORBELL_OFF_WRCH_0), 0x1);

	return true;
}

/* read transfer from the prespective of the chip. i.e., host to chip */
bool PcieDmaReadTransfer(uint64_t chip_addr, uint64_t host_addr, uint32_t transfer_size_bytes,
			 uint64_t msi_completion_addr, uint8_t completion_data)
{
	/* reject dma request if there is a pending transaction */
	uint32_t status = ReadDbiReg(HDMA_REG_ADDR(STATUS_OFF_RDCH_0));

	if (status == DMARunning) {
		return false;
	}

	/* Setup completion interrupt */
	BH_PCIE_DWC_PCIE_USP_PF0_HDMA_CAP_HDMA_INT_SETUP_OFF_RDCH_0_reg_u int_setup;

	int_setup.val = 0;
	int_setup.f.rsie = 1;
	int_setup.f.raie = 1;
	WriteDbiReg(HDMA_REG_ADDR(INT_SETUP_OFF_RDCH_0), int_setup.val);
	WriteDbiReg(HDMA_REG_ADDR(MSI_STOP_LOW_OFF_RDCH_0), low32(msi_completion_addr));
	WriteDbiReg(HDMA_REG_ADDR(MSI_STOP_HIGH_OFF_RDCH_0), high32(msi_completion_addr));
	WriteDbiReg(HDMA_REG_ADDR(MSI_ABORT_LOW_OFF_RDCH_0),
		    low32(msi_completion_addr + sizeof(uint32_t)));
	WriteDbiReg(HDMA_REG_ADDR(MSI_ABORT_HIGH_OFF_RDCH_0),
		    high32(msi_completion_addr + sizeof(uint32_t)));
	WriteDbiReg(HDMA_REG_ADDR(MSI_MSGD_OFF_RDCH_0), completion_data);

	/* Enable read channel 0 */
	WriteDbiReg(HDMA_REG_ADDR(EN_OFF_RDCH_0), 0x1);

	WriteDbiReg(HDMA_REG_ADDR(SAR_LOW_OFF_RDCH_0), low32(host_addr));
	WriteDbiReg(HDMA_REG_ADDR(SAR_HIGH_OFF_RDCH_0), high32(host_addr));
	WriteDbiReg(HDMA_REG_ADDR(DAR_LOW_OFF_RDCH_0), low32(chip_addr));
	WriteDbiReg(HDMA_REG_ADDR(DAR_HIGH_OFF_RDCH_0), high32(chip_addr));
	WriteDbiReg(HDMA_REG_ADDR(XFERSIZE_OFF_RDCH_0), transfer_size_bytes);
	WriteDbiReg(HDMA_REG_ADDR(DOORBELL_OFF_RDCH_0), 0x1);

	return true;
}

/**
 * @brief Handler for @ref TT_SMC_MSG_PCIE_DMA_CHIP_TO_HOST_TRANSFER and
 *        @ref TT_SMC_MSG_PCIE_DMA_HOST_TO_CHIP_TRANSFER
 * @see pcie_dma_transfer_rqst
 */
static uint8_t pcie_dma_transfer_handler(const union request *request, struct response *response)
{
	uint8_t completion_data = request->pcie_dma_transfer.completion_data;
	uint32_t transfer_size_bytes = request->pcie_dma_transfer.transfer_size_bytes;
	uint64_t chip_addr = request->pcie_dma_transfer.chip_addr;
	uint64_t host_addr = request->pcie_dma_transfer.host_addr;
	uint64_t msi_completion_addr = request->pcie_dma_transfer.msi_completion_addr;

	bool accept;

	if (request->command_code == TT_SMC_MSG_PCIE_DMA_HOST_TO_CHIP_TRANSFER) {
		accept = PcieDmaReadTransfer(chip_addr, host_addr, transfer_size_bytes,
					     msi_completion_addr, completion_data);
	} else {
		accept = PcieDmaWriteTransfer(chip_addr, host_addr, transfer_size_bytes,
					      msi_completion_addr, completion_data);
	}

	return accept ? 0 : 1;
}

REGISTER_MESSAGE(TT_SMC_MSG_PCIE_DMA_HOST_TO_CHIP_TRANSFER, pcie_dma_transfer_handler);
REGISTER_MESSAGE(TT_SMC_MSG_PCIE_DMA_CHIP_TO_HOST_TRANSFER, pcie_dma_transfer_handler);
