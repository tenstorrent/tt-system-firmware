/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <tenstorrent/smc_msg.h>
#include <tenstorrent/msgqueue.h>

#include "pcie.h"

#define BH_PCIE_DWC_PCIE_USP_PF0_MSI_CAP_PCI_MSI_CAP_ID_NEXT_CTRL_REG_REG_ADDR 0x00000050
#define BH_PCIE_DWC_PCIE_USP_PF0_MSI_CAP_MSI_CAP_OFF_04H_REG_REG_ADDR          0x00000054
#define BH_PCIE_DWC_PCIE_USP_PF0_MSI_CAP_MSI_CAP_OFF_08H_REG_REG_ADDR          0x00000058
#define BH_PCIE_DWC_PCIE_USP_PF0_MSI_CAP_MSI_CAP_OFF_0CH_REG_REG_ADDR          0x0000005C

typedef struct {
	uint32_t pci_msi_cap_id: 8;
	uint32_t pci_msi_cap_next_offset: 8;
	uint32_t pci_msi_enable: 1;
	uint32_t pci_msi_multiple_msg_cap: 3;
	uint32_t pci_msi_multiple_msg_en: 3;
	uint32_t pci_msi_64_bit_addr_cap: 1;
	uint32_t pci_pvm_support: 1;
	uint32_t pci_msi_ext_data_cap: 1;
	uint32_t pci_msi_ext_data_en: 1;
	uint32_t rsvdp_27: 5;
} BH_PCIE_DWC_PCIE_USP_PF0_MSI_CAP_HDL_PATH_E982B20F_PCI_MSI_CAP_ID_NEXT_CTRL_REG_reg_t;

typedef union {
	uint32_t val;
	BH_PCIE_DWC_PCIE_USP_PF0_MSI_CAP_HDL_PATH_E982B20F_PCI_MSI_CAP_ID_NEXT_CTRL_REG_reg_t f;
} BH_PCIE_DWC_PCIE_USP_PF0_MSI_CAP_HDL_PATH_E982B20F_PCI_MSI_CAP_ID_NEXT_CTRL_REG_reg_u;

#define BH_PCIE_DWC_PCIE_USP_PF0_MSI_CAP_HDL_PATH_E982B20F_PCI_MSI_CAP_ID_NEXT_CTRL_REG_REG_DEFAULT \
	(0x01807005)

uint32_t GetVectorsAllowed(uint32_t mult_msg_en)
{
	return 1 << mult_msg_en;
}

void SendPcieMsi(uint8_t pcie_inst, uint32_t vector_id)
{
	BH_PCIE_DWC_PCIE_USP_PF0_MSI_CAP_HDL_PATH_E982B20F_PCI_MSI_CAP_ID_NEXT_CTRL_REG_reg_u
		pci_msi_cap;
	pci_msi_cap.val =
		ReadDbiReg(BH_PCIE_DWC_PCIE_USP_PF0_MSI_CAP_PCI_MSI_CAP_ID_NEXT_CTRL_REG_REG_ADDR);
	uint32_t vectors_allowed = GetVectorsAllowed(pci_msi_cap.f.pci_msi_multiple_msg_en);

	if (pci_msi_cap.f.pci_msi_enable && vector_id < vectors_allowed) {
		uint32_t msi_addr_lo =
			ReadDbiReg(BH_PCIE_DWC_PCIE_USP_PF0_MSI_CAP_MSI_CAP_OFF_04H_REG_REG_ADDR);
		uint32_t msi_addr_hi =
			ReadDbiReg(BH_PCIE_DWC_PCIE_USP_PF0_MSI_CAP_MSI_CAP_OFF_08H_REG_REG_ADDR);
		uint64_t msi_addr = ((uint64_t)msi_addr_hi << 32) | msi_addr_lo;
		uint32_t msi_data =
			ReadDbiReg(BH_PCIE_DWC_PCIE_USP_PF0_MSI_CAP_MSI_CAP_OFF_0CH_REG_REG_ADDR);
		msi_data += vector_id;

		const uint8_t ring = 0;
		const uint8_t tlb_num = 12;
		const uint8_t x = pcie_inst == 0 ? PCIE_INST0_LOGICAL_X : PCIE_INST1_LOGICAL_X;
		const uint8_t y = PCIE_LOGICAL_Y;

		NOC2AXITlbSetup(ring, tlb_num, x, y, msi_addr);
		NOC2AXIWrite32(ring, tlb_num, msi_addr, msi_data);
	}
}

/**
 * @brief Handler for @ref TT_SMC_MSG_SEND_PCIE_MSI messages
 *
 * @details Sends a PCIe Message Signaled Interrupt (MSI) with the specified
 *          vector ID on the given PCIe instance.
 *
 * @param request Pointer to the host request message to be processed
 * @param response Pointer to the response message to be sent back to host
 *
 * @return 0 always
 *
 * @see send_pcie_msi_rqst
 *
 */
static uint8_t send_pcie_msi_handler(const union request *request, struct response *response)
{
	uint8_t pcie_inst = request->send_pci_msi.pcie_inst;
	uint32_t vector_id = request->send_pci_msi.vector_id;

	SendPcieMsi(pcie_inst, vector_id);
	return 0;
}

REGISTER_MESSAGE(TT_SMC_MSG_SEND_PCIE_MSI, send_pcie_msi_handler);
