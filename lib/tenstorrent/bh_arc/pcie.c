/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cm2dm_msg.h"
#include "init.h"
#include "irqnum.h"
#include "noc2axi.h"
#include "pcie.h"
#include "pciesd.h"
#include "reg.h"
#include "status_reg.h"
#include "timer.h"

#include "arc_dma.h"

#include <stdbool.h>

#include <tenstorrent/post_code.h>
#include <tenstorrent/sys_init_defines.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/misc/bh_fwtable.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/dma_arc_hs.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

/* FIXME: should be done via devicetree */
#define PCIE_BAR0_SIZE_DEFAULT_MB 512
#define PCIE_BAR2_SIZE_DEFAULT_MB 1
#define PCIE_BAR4_SIZE_DEFAULT_MB 32768

#define PCIE_SERDES0_ALPHACORE_TLB 0
#define PCIE_SERDES1_ALPHACORE_TLB 1
#define PCIE_SERDES0_CTRL_TLB      2
#define PCIE_SERDES1_CTRL_TLB      3
#define PCIE_SII_REG_TLB           4
#define PCIE_TLB_CONFIG_TLB        5

#define SERDES_INST_OFFSET         0x04000000
#define PCIE_SERDES_SOC_REG_OFFSET 0x03000000
#define PCIE_TLB_CONFIG_ADDR       0x1FC00000

#define DBI_PCIE_TLB_ID                   62
#define PCIE_NOC_TLB_DATA_REG_OFFSET2(ID) PCIE_SII_A_NOC_TLB_DATA_##ID##__REG_OFFSET
#define PCIE_NOC_TLB_DATA_REG_OFFSET(ID)  PCIE_NOC_TLB_DATA_REG_OFFSET2(ID)
#define DBI_ADDR                          ((uint64_t)DBI_PCIE_TLB_ID << 58)

#define CMN_A_REG_MAP_BASE_ADDR         0xFFFFFFFFE1000000LL
#define SERDES_SS_0_A_REG_MAP_BASE_ADDR 0xFFFFFFFFE0000000LL
#define PCIE_SII_A_REG_MAP_BASE_ADDR    0xFFFFFFFFF0000000LL

#define PCIE_SII_A_NOC_TLB_DATA_62__REG_OFFSET 0x0000022C
#define PCIE_SII_A_NOC_TLB_DATA_0__REG_OFFSET  0x00000134
#define PCIE_SII_A_APP_PCIE_CTL_REG_OFFSET     0x0000005C
#define PCIE_SII_A_LTSSM_STATE_REG_OFFSET      0x00000128

LOG_MODULE_DECLARE(bh_arc);

static const struct device *const fwtable_dev = DEVICE_DT_GET(DT_NODELABEL(fwtable));
static const struct device *const arc_dma_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(dma0));

/* Verify prototype of ArcDmaTransfer, because it's used by libpciesd.a. */
static __unused bool (*verify_ArcDmaTransfer)(const void *, void *, uint32_t) = ArcDmaTransfer;

typedef struct {
	uint32_t tlp_type: 5;
	uint32_t ser_np: 1;
	uint32_t ep: 1;
	uint32_t rsvd_0: 1;
	uint32_t ns: 1;
	uint32_t ro: 1;
	uint32_t tc: 3;
	uint32_t msg: 8;
	uint32_t dbi: 1;
	uint32_t atu_bypass: 1;
	uint32_t addr: 6;
} PCIE_SII_NOC_TLB_DATA_reg_t;

typedef union {
	uint32_t val;
	PCIE_SII_NOC_TLB_DATA_reg_t f;
} PCIE_SII_NOC_TLB_DATA_reg_u;

#define PCIE_SII_NOC_TLB_DATA_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t app_hold_phy_rst_axiclk: 1;
	uint32_t app_l1sub_disable_axiclk: 1;
	uint32_t app_margining_ready_axiclk: 1;
	uint32_t app_margining_software_ready_axiclk: 1;
	uint32_t app_pf_req_retry_en_axiclk: 1;
	uint32_t app_clk_req_n_axiclk: 1;
	uint32_t phy_clk_req_n_axiclk: 1;
	uint32_t rsvd_0: 23;
	uint32_t slv_rasdp_err_mode: 1;
	uint32_t mstr_rasdp_err_mode: 1;
} PCIE_SII_APP_PCIE_CTL_reg_t;

typedef union {
	uint32_t val;
	PCIE_SII_APP_PCIE_CTL_reg_t f;
} PCIE_SII_APP_PCIE_CTL_reg_u;

#define PCIE_SII_APP_PCIE_CTL_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t smlh_ltssm_state_sync: 6;
	uint32_t rdlh_link_up_sync: 1;
	uint32_t smlh_link_up_sync: 1;
} PCIE_SII_LTSSM_STATE_reg_t;

typedef union {
	uint32_t val;
	PCIE_SII_LTSSM_STATE_reg_t f;
} PCIE_SII_LTSSM_STATE_reg_u;

#define PCIE_SII_LTSSM_STATE_REG_DEFAULT (0x00000000)

static const struct device *gpio3 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpio3));

static inline void WritePcieTlbConfigReg(const uint32_t addr, const uint32_t data)
{
	const uint8_t noc_id = 0;

	NOC2AXIWrite32(noc_id, PCIE_TLB_CONFIG_TLB, addr, data);
}

static inline void WriteDbiRegByte(const uint32_t addr, const uint8_t data)
{
	const uint8_t noc_id = 0;

	NOC2AXIWrite8(noc_id, PCIE_DBI_REG_TLB, addr, data);
}

static inline void WriteSiiReg(const uint32_t addr, const uint32_t data)
{
	const uint8_t noc_id = 0;

	NOC2AXIWrite32(noc_id, PCIE_SII_REG_TLB, addr, data);
}

static inline uint32_t ReadSiiReg(const uint32_t addr)
{
	const uint8_t noc_id = 0;

	return NOC2AXIRead32(noc_id, PCIE_SII_REG_TLB, addr);
}

static inline void WriteSerdesAlphaCoreReg(const uint8_t inst, const uint32_t addr,
					   const uint32_t data)
{
	const uint8_t noc_id = 0;
	uint8_t tlb = (inst == 0) ? PCIE_SERDES0_ALPHACORE_TLB : PCIE_SERDES1_ALPHACORE_TLB;

	NOC2AXIWrite32(noc_id, tlb, addr, data);
}

static inline uint32_t ReadSerdesAlphaCoreReg(const uint8_t inst, const uint32_t addr)
{
	const uint8_t noc_id = 0;
	uint8_t tlb = (inst == 0) ? PCIE_SERDES0_ALPHACORE_TLB : PCIE_SERDES1_ALPHACORE_TLB;

	return NOC2AXIRead32(noc_id, tlb, addr);
}

static inline void WriteSerdesCtrlReg(const uint8_t inst, const uint32_t addr, const uint32_t data)
{
	const uint8_t noc_id = 0;
	uint8_t tlb = (inst == 0) ? PCIE_SERDES0_CTRL_TLB : PCIE_SERDES1_CTRL_TLB;

	NOC2AXIWrite32(noc_id, tlb, addr, data);
}

/* Wrapper for ARC DMA, used by libpciesd.a */
bool ArcDmaTransfer(const void *src, void *dst, uint32_t len)
{
	if (arc_dma_dev == NULL) {
		return false;
	}
	return dma_arc_hs_transfer(arc_dma_dev, 0, src, dst, len, K_MSEC(500)) == 0;
}

static inline void SetupDbiAccess(void)
{
	PCIE_SII_NOC_TLB_DATA_reg_u noc_tlb_data_reg;

	noc_tlb_data_reg.val = PCIE_SII_NOC_TLB_DATA_REG_DEFAULT;
	noc_tlb_data_reg.f.dbi = 1;
	WriteSiiReg(PCIE_NOC_TLB_DATA_REG_OFFSET(DBI_PCIE_TLB_ID), noc_tlb_data_reg.val);
	/* flush out NOC_TLB_DATA register so that subsequent dbi writes are mapped to the correct
	 * location
	 */
	ReadSiiReg(PCIE_NOC_TLB_DATA_REG_OFFSET(DBI_PCIE_TLB_ID));
}

static void CntlInitV2ParamInit(uint8_t pcie_inst, const ReadOnly *rotable,
				const struct _FwTable_PciPropertyTable *pcitable,
				struct CntlInitV2Param *param)
{
	/* Start with 32-bit bar size in MiB. Round up as needed. Final value is bar mask in B */
	uint64_t bar_sizes[] = {
		pcitable->pcie_bar0_size,
		pcitable->pcie_bar2_size,
		pcitable->pcie_bar4_size,
	};

	if (pcitable->pcie_bar0_size != PCIE_BAR0_SIZE_DEFAULT_MB) {
		LOG_WRN("BAR%zu %s(%u -> size %llu MiB)", 0, "Fixed ", (uint32_t)bar_sizes[0],
			(uint64_t)PCIE_BAR0_SIZE_DEFAULT_MB);
		bar_sizes[0] = PCIE_BAR0_SIZE_DEFAULT_MB;
	} else {
		LOG_INF("BAR%zu %s(%u -> size %llu MiB)", 0, "", PCIE_BAR0_SIZE_DEFAULT_MB,
			(uint64_t)PCIE_BAR0_SIZE_DEFAULT_MB);
	}
	/* convert to bytes and adjust by -1 to get the correct mask */
	bar_sizes[0] *= MB(1);
	bar_sizes[0] -= 1;

	if (pcitable->pcie_bar2_size != PCIE_BAR2_SIZE_DEFAULT_MB) {
		LOG_WRN("BAR%zu %s(%u -> size %llu MiB)", 2, "Fixed ", (uint32_t)bar_sizes[1],
			(uint64_t)PCIE_BAR2_SIZE_DEFAULT_MB);
		bar_sizes[1] = PCIE_BAR2_SIZE_DEFAULT_MB;
	} else {
		LOG_INF("BAR%zu %s(%u -> size %llu MiB)", 2, "", PCIE_BAR2_SIZE_DEFAULT_MB,
			(uint64_t)PCIE_BAR2_SIZE_DEFAULT_MB);
	}
	/* convert to bytes and adjust by -1 to get the correct mask */
	bar_sizes[1] *= MB(1);
	bar_sizes[1] -= 1;

	if ((uint32_t)bar_sizes[2] == 0) {
		LOG_WRN("BAR%zu %s(%u -> size %llu MiB)", 4, "Disabled ", 0U, 0ULL);
	} else {
		if (!IS_POWER_OF_TWO(bar_sizes[2])) {
			uint64_t nhpot = NHPOT(bar_sizes[2]);

			LOG_WRN("BAR%zu %s(%u -> size %llu MiB)", 4, "Rounded-up ",
				(uint32_t)bar_sizes[2], nhpot);
			bar_sizes[2] = nhpot;
		} else {
			LOG_INF("BAR%zu %s(%u -> size %llu MiB)", 4, "", (uint32_t)bar_sizes[2],
				bar_sizes[2]);
		}
		/* convert to bytes and adjust by -1 to get the correct mask */
		bar_sizes[2] *= MB(1);
		bar_sizes[2] -= 1;
	}

	*param = (struct CntlInitV2Param){
		.board_id = rotable->board_id,
		.vendor_id = rotable->vendor_id,
		.num_serdes_instance = pcitable->num_serdes,
		.max_pcie_speed = pcitable->max_pcie_speed,
		.pcie_inst = pcie_inst,
		/* pcie_mode - 1 to match with definition in pcie.h for PCIeDeviceType */
		.device_type = pcitable->pcie_mode - 1,
		.region0_mask = bar_sizes[0],
		.region2_mask = bar_sizes[1],
		.region4_mask = bar_sizes[2],
	};
}

static void InitResetInterrupt(uint8_t pcie_inst)
{
#if CONFIG_ARC
	if (pcie_inst == 0) {
		IRQ_CONNECT(IRQNUM_PCIE0_ERR_INTR, 0, ChipResetRequest, IRQNUM_PCIE0_ERR_INTR, 0);
		irq_enable(IRQNUM_PCIE0_ERR_INTR);
	} else if (pcie_inst == 1) {
		IRQ_CONNECT(IRQNUM_PCIE1_ERR_INTR, 0, ChipResetRequest, IRQNUM_PCIE1_ERR_INTR, 0);
		irq_enable(IRQNUM_PCIE1_ERR_INTR);
	}
#else
	ARG_UNUSED(pcie_inst);
#endif
}

static void SetupOutboundTlbs(void)
{
	static const PCIE_SII_NOC_TLB_DATA_reg_t tlb_settings[] = {
		{
			.atu_bypass = 1,
		},
		{
			.atu_bypass = 1,
			.ro = 1,
		},
		{
			.atu_bypass = 1,
			.ns = 1,
		},
		{
			.atu_bypass = 1,
			.ro = 1,
			.ns = 1,
		},
		{},
		{
			.ro = 1,
		},
		{
			.ns = 1,
		},
		{
			.ro = 1,
			.ns = 1,
		},
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(tlb_settings); i++) {
		PCIE_SII_NOC_TLB_DATA_reg_u reg = {.f = tlb_settings[i]};
		uint32_t addr = PCIE_NOC_TLB_DATA_REG_OFFSET(0) + sizeof(uint32_t) * i;

		WriteSiiReg(addr, reg.val);
	}

	ReadSiiReg(PCIE_NOC_TLB_DATA_REG_OFFSET(0)); /* Stall until writes have completed. */
}

static void ConfigurePCIeTlbs(uint8_t pcie_inst)
{
	const uint8_t ring = 0;
	const uint8_t ring0_logic_x = pcie_inst == 0 ? PCIE_INST0_LOGICAL_X : PCIE_INST1_LOGICAL_X;
	const uint8_t ring0_logic_y = PCIE_LOGICAL_Y;

	NOC2AXITlbSetup(ring, PCIE_SERDES0_ALPHACORE_TLB, ring0_logic_x, ring0_logic_y,
			CMN_A_REG_MAP_BASE_ADDR);
	NOC2AXITlbSetup(ring, PCIE_SERDES1_ALPHACORE_TLB, ring0_logic_x, ring0_logic_y,
			CMN_A_REG_MAP_BASE_ADDR + SERDES_INST_OFFSET);
	NOC2AXITlbSetup(ring, PCIE_SERDES0_CTRL_TLB, ring0_logic_x, ring0_logic_y,
			SERDES_SS_0_A_REG_MAP_BASE_ADDR + PCIE_SERDES_SOC_REG_OFFSET);
	NOC2AXITlbSetup(ring, PCIE_SERDES1_CTRL_TLB, ring0_logic_x, ring0_logic_y,
			SERDES_SS_0_A_REG_MAP_BASE_ADDR + SERDES_INST_OFFSET +
				PCIE_SERDES_SOC_REG_OFFSET);
	NOC2AXITlbSetup(ring, PCIE_SII_REG_TLB, ring0_logic_x, ring0_logic_y,
			PCIE_SII_A_REG_MAP_BASE_ADDR);
	NOC2AXITlbSetup(ring, PCIE_DBI_REG_TLB, ring0_logic_x, ring0_logic_y, DBI_ADDR);
	NOC2AXITlbSetup(ring, PCIE_TLB_CONFIG_TLB, ring0_logic_x, ring0_logic_y,
			PCIE_TLB_CONFIG_ADDR);
}

static void SetupInboundTlbs(void)
{
	EnterLoopback();
	WaitMs(1);
	/* Configure inbound 4G TLB window to point at 8,3,0x4000_0000_0000 */
	WritePcieTlbConfigReg(0x1fc00978, 0x4000);
	WritePcieTlbConfigReg(0x1fc0097c, 0x00c8);
	WritePcieTlbConfigReg(0x1fc00980, 0x0000);
	ExitLoopback();
}

static void SetupSii(void)
{
	/* For GEN4 lane margining, spec requires app_margining_ready = 1 and
	 * app_margining_software_ready = 0
	 */
	PCIE_SII_APP_PCIE_CTL_reg_u app_pcie_ctl;

	app_pcie_ctl.val = PCIE_SII_APP_PCIE_CTL_REG_DEFAULT;
	app_pcie_ctl.f.app_margining_ready_axiclk = 1;
	WriteSiiReg(PCIE_SII_A_APP_PCIE_CTL_REG_OFFSET, app_pcie_ctl.val);
}

static PCIeInitStatus PCIeInitComm(const struct CntlInitV2Param *param)
{
	ConfigurePCIeTlbs(param->pcie_inst);

	PCIeInitStatus status =
		SerdesInit(param->pcie_inst, param->device_type, param->num_serdes_instance);

	if (status != PCIeInitOk) {
		return status;
	}

	SetupDbiAccess();
	CntlInitV2(param);

	SetupSii();
	SetupOutboundTlbs(); /* pcie_inst is implied by ConfigurePCIeTlbs */
	return status;
}

static void TogglePerst(void)
{
	/* GPIO34 is TRISTATE of level shifter, GPIO37 is PERST input to the level shifter */
	gpio_pin_configure(gpio3, 2, GPIO_OUTPUT);
	gpio_pin_configure(gpio3, 5, GPIO_OUTPUT);
	gpio_pin_configure(gpio3, 7, GPIO_OUTPUT);

	/* put device into reset for 1 ms */
	gpio_pin_set(gpio3, 2, 1);
	gpio_pin_set(gpio3, 5, 0);
	gpio_pin_set(gpio3, 7, 0);
	WaitMs(1);

	/* take device out of reset */
	gpio_pin_set(gpio3, 5, 1);
	gpio_pin_set(gpio3, 7, 1);
}

static PCIeInitStatus PollForLinkUp(uint8_t pcie_inst)
{
	ARG_UNUSED(pcie_inst);

	/* timeout after 200 ms */
	uint64_t end_time = TimerTimestamp() + 500 * WAIT_1MS;
	bool training_done = false;

	do {
		PCIE_SII_LTSSM_STATE_reg_u ltssm_state;

		ltssm_state.val = ReadSiiReg(PCIE_SII_A_LTSSM_STATE_REG_OFFSET);
		training_done = ltssm_state.f.smlh_link_up_sync && ltssm_state.f.rdlh_link_up_sync;
	} while (!training_done && TimerTimestamp() < end_time);

	if (!training_done) {
		return PCIeLinkTrainTimeout;
	}

	return PCIeInitOk;
}

static PCIeInitStatus PCIeInit(const struct CntlInitV2Param *param)
{
	if ((PCIeDeviceType)param->device_type == RootComplex) {
		TogglePerst();
	}

	PCIeInitStatus status = PCIeInitComm(param);

	if (status != PCIeInitOk) {
		return status;
	}

	if ((PCIeDeviceType)param->device_type == RootComplex) {
		status = PollForLinkUp(param->pcie_inst);
		if (status != PCIeInitOk) {
			return status;
		}

		SetupInboundTlbs();

		/* re-initialize PCIe link */
		TogglePerst();
		status = PCIeInitComm(param);
	}

	return status;
}

static int pcie_init(void)
{
	/* Initialize the serdes based on board type and asic location - data will be in fw_table */
	/* p100: PCIe1 x16 */
	/* p150: PCIe0 x16 */
	/* p300: Left (CPU1) PCIe1 x8, Right (CPU0) PCIe0 x8 */
	/* BH UBB: PCIe1 x8 */
	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEP8);

	if (!IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	const ReadOnly *rotable = tt_bh_fwtable_get_read_only_table(fwtable_dev);
	FwTable_PciPropertyTable pci0_property_table;
	FwTable_PciPropertyTable pci1_property_table;
	struct CntlInitV2Param param;

	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY)) {
		/* Set num_serdes based on board type:
		 * P300 variants and UBB (Galaxy): 1 serdes
		 * P100 and P150 variants: 2 serdes
		 */
#if defined(CONFIG_BOARD_REVISION_P300A) || defined(CONFIG_BOARD_REVISION_P300B) ||                \
	defined(CONFIG_BOARD_REVISION_P300C) || defined(CONFIG_BOARD_REVISION_GALAXY) ||           \
	defined(CONFIG_BOARD_REVISION_GALAXY_REVC)
		uint8_t recovery_num_serdes = 1;
#else
		uint8_t recovery_num_serdes = 2;
#endif

		pci0_property_table = (FwTable_PciPropertyTable){
			.pcie_mode = FwTable_PciPropertyTable_PcieMode_EP,
			.num_serdes = recovery_num_serdes,
			.pcie_bar0_size = PCIE_BAR0_SIZE_DEFAULT_MB,
			.pcie_bar2_size = PCIE_BAR2_SIZE_DEFAULT_MB,
			.pcie_bar4_size = PCIE_BAR4_SIZE_DEFAULT_MB,
		};
		pci1_property_table = (FwTable_PciPropertyTable){
			.pcie_mode = FwTable_PciPropertyTable_PcieMode_EP,
			.num_serdes = recovery_num_serdes,
			.pcie_bar0_size = PCIE_BAR0_SIZE_DEFAULT_MB,
			.pcie_bar2_size = PCIE_BAR2_SIZE_DEFAULT_MB,
			.pcie_bar4_size = PCIE_BAR4_SIZE_DEFAULT_MB,
		};
	} else {
		pci0_property_table = tt_bh_fwtable_get_fw_table(fwtable_dev)->pci0_property_table;
		pci1_property_table = tt_bh_fwtable_get_fw_table(fwtable_dev)->pci1_property_table;
	}

	if (pci0_property_table.pcie_mode != FwTable_PciPropertyTable_PcieMode_DISABLED) {
		CntlInitV2ParamInit(0, rotable, &pci0_property_table, &param);
		PCIeInit(&param);
	}

	if (pci1_property_table.pcie_mode != FwTable_PciPropertyTable_PcieMode_DISABLED) {
		CntlInitV2ParamInit(1, rotable, &pci1_property_table, &param);
		PCIeInit(&param);
	}

	InitResetInterrupt(0);
	InitResetInterrupt(1);

	WriteReg(PCIE_INIT_CPL_TIME_REG_ADDR, TimerTimestamp());

	return 0;
}
SYS_INIT_APP(pcie_init);
