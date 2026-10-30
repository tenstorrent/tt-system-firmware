/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "chip_info.h"
#include "cm2dm_msg.h"
#include "init.h"
#include "irqnum.h"
#include "noc2axi.h"
#include "pcie.h"
#include "pcie_ltssm_log.h"
#include "pciesd.h"
#include "reg.h"
#include "status_reg.h"
#include "timer.h"

#include <inttypes.h>
#include "arc_dma.h"

#include <stdbool.h>

#include <tenstorrent/post_code.h>
#include <tenstorrent/sys_init_defines.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/dma_arc_hs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#define PCIE_SERDES0_ALPHACORE_TLB 0
#define PCIE_SERDES1_ALPHACORE_TLB 1
#define PCIE_SERDES0_CTRL_TLB      2
#define PCIE_SERDES1_CTRL_TLB      3
#define PCIE_SII_REG_TLB           4
#define PCIE_TLB_CONFIG_TLB        5
/* Owned by the LTSSM recorder: eth, throttler and DMA reprogram the shared TLBs at runtime */
#define PCIE_LTSSM_SII_TLB         6
#define PCIE_LTSSM_PHY0_TLB        7 /* CDR option only, one per SerDes */
#define PCIE_LTSSM_PHY1_TLB        8

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

/* AlphaCore dig_soc_lane_stat_reg1; octl_rx_data_vld is the per-lane RX CDR lock */
#define PHY_LANE_STAT_REG1_OFFSET 0x00013038
#define PHY_LANE_STRIDE           0x00010000
#define PHY_LANES_PER_SERDES      8
#define PHY_OCTL_RX_DATA_VLD      BIT(1)

LOG_MODULE_DECLARE(bh_arc);

static const struct device *const arc_dma_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(dma0));

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

static void CntlInitV3ParamInit(uint8_t pcie_inst, uint64_t board_id, uint32_t vendor_id,
				const struct bh_pci_property *pcitable,
				struct CntlInitV3Param *param)
{
	/* Start with 32-bit bar size in MiB. Round up as needed. Final value is bar mask in B */
	uint64_t bar_sizes[] = {
		pcitable->pcie_bar0_size,
		pcitable->pcie_bar2_size,
		pcitable->pcie_bar4_size,
	};

	if (pcitable->pcie_bar0_size != PCIE_BAR0_SIZE_DEFAULT_MB) {
		LOG_WRN("BAR0 Fixed (%" PRIu64 " -> size %" PRIu64 " MiB)", bar_sizes[0],
			(uint64_t)PCIE_BAR0_SIZE_DEFAULT_MB);
		bar_sizes[0] = PCIE_BAR0_SIZE_DEFAULT_MB;
	} else {
		LOG_INF("BAR0 (%u -> size %" PRIu64 " MiB)", PCIE_BAR0_SIZE_DEFAULT_MB,
			(uint64_t)PCIE_BAR0_SIZE_DEFAULT_MB);
	}
	/* convert to bytes and adjust by -1 to get the correct mask */
	bar_sizes[0] *= MB(1);
	bar_sizes[0] -= 1;

	if (pcitable->pcie_bar2_size != PCIE_BAR2_SIZE_DEFAULT_MB) {
		LOG_WRN("BAR2 Fixed (%" PRIu64 " -> size %" PRIu64 " MiB)", bar_sizes[1],
			(uint64_t)PCIE_BAR2_SIZE_DEFAULT_MB);
		bar_sizes[1] = PCIE_BAR2_SIZE_DEFAULT_MB;
	} else {
		LOG_INF("BAR2 (%u -> size %" PRIu64 " MiB)", PCIE_BAR2_SIZE_DEFAULT_MB,
			(uint64_t)PCIE_BAR2_SIZE_DEFAULT_MB);
	}
	/* convert to bytes and adjust by -1 to get the correct mask */
	bar_sizes[1] *= MB(1);
	bar_sizes[1] -= 1;

	if ((uint32_t)bar_sizes[2] == 0) {
		LOG_WRN("BAR4 Disabled (0 -> size 0 MiB)");
	} else {
		if (!IS_POWER_OF_TWO(bar_sizes[2])) {
			uint64_t nhpot = NHPOT(bar_sizes[2]);

			LOG_WRN("BAR4 Rounded-up (%" PRIu64 " -> size %" PRIu64 " MiB)",
				bar_sizes[2], nhpot);
			bar_sizes[2] = nhpot;
		} else {
			LOG_INF("BAR4 (%" PRIu64 " -> size %" PRIu64 " MiB)", bar_sizes[2],
				bar_sizes[2]);
		}
		/* convert to bytes and adjust by -1 to get the correct mask */
		bar_sizes[2] *= MB(1);
		bar_sizes[2] -= 1;
	}

	*param = (struct CntlInitV3Param){
		.board_id = board_id,
		.vendor_id = vendor_id,
		.num_serdes_instance = pcitable->num_serdes,
		.max_pcie_speed = pcitable->max_pcie_speed,
		.pcie_inst = pcie_inst,
		/* pcie_mode - 1 to match with definition in pciesd.h for PCIeDeviceType */
		.device_type = pcitable->pcie_mode - 1,
		.region0_mask = bar_sizes[0],
		.region2_mask = bar_sizes[1],
		.region4_mask = bar_sizes[2],
		.gen3_eq_pset_req_vec = pcitable->gen3_eq_pset_req_vec,
		.gen3_eq_fb_mode = pcitable->gen3_eq_fb_mode,
		.gen4_eq_pset_req_vec = pcitable->gen4_eq_pset_req_vec,
		.gen5_eq_pset_req_vec = pcitable->gen5_eq_pset_req_vec,
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

static PCIeInitStatus PCIeInitComm(const struct CntlInitV3Param *param)
{
	ConfigurePCIeTlbs(param->pcie_inst);

	PCIeInitStatus status =
		SerdesInit(param->pcie_inst, param->device_type, param->num_serdes_instance);

	if (status != PCIeInitOk) {
		return status;
	}

	SetupDbiAccess();
	CntlInitV3(param);

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

static PCIeInitStatus PCIeInit(const struct CntlInitV3Param *param)
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

#ifdef CONFIG_TT_BH_ARC_PCIE_LTSSM_LOG
/* TimerTimestamp() can tear on a carry between its halves; the smaller of two reads is good */
static uint64_t LtssmTimestamp(void)
{
	uint64_t first = TimerTimestamp();
	uint64_t second = TimerTimestamp();

	return second < first ? second : first;
}

#ifdef CONFIG_TT_BH_ARC_PCIE_LTSSM_LOG_CDR
static uint32_t ReadCdrLock(uint8_t lanes)
{
	uint32_t bitmap = 0;

	for (uint8_t lane = 0; lane < lanes; lane++) {
		uint8_t tlb =
			lane < PHY_LANES_PER_SERDES ? PCIE_LTSSM_PHY0_TLB : PCIE_LTSSM_PHY1_TLB;
		uint32_t offset =
			PHY_LANE_STAT_REG1_OFFSET + (lane % PHY_LANES_PER_SERDES) * PHY_LANE_STRIDE;

		if (NOC2AXIRead32(0, tlb, offset) & PHY_OCTL_RX_DATA_VLD) {
			bitmap |= BIT(lane);
		}
	}

	return bitmap | ((uint32_t)lanes << LTSSM_CDR_LANES_SHIFT);
}

/* S_DETECT_QUIET, S_DETECT_ACT, S_PRE_DETECT_QUIET and S_DETECT_WAIT */
#define LTSSM_DETECT_STATES (BIT64(0x00) | BIT64(0x01) | BIT64(0x05) | BIT64(0x06))
#endif /* CONFIG_TT_BH_ARC_PCIE_LTSSM_LOG_CDR */

/* Never returns: busy-polls because recovery transitions can be 600 ns apart */
static void CaptureLtssmTraining(uint8_t inst_mask, const uint8_t *lanes)
{
	uint8_t last_state[2] = {LTSSM_STATE_NONE, LTSSM_STATE_NONE};
	uint8_t last_link[2] = {0, 0};
	uint32_t last_cdr[2] = {0, 0};
#ifdef CONFIG_TT_BH_ARC_PCIE_LTSSM_LOG_CDR
	uint32_t poll_div[2] = {0, 0};
#else
	ARG_UNUSED(lanes);
#endif
	int8_t tlb_inst = -1;

	while (true) {
		for (uint8_t inst = 0; inst < 2; inst++) {
			if ((inst_mask & BIT(inst)) == 0) {
				continue;
			}

			if (tlb_inst != (int8_t)inst) {
				uint8_t x = inst == 0 ? PCIE_INST0_LOGICAL_X : PCIE_INST1_LOGICAL_X;

				NOC2AXITlbSetup(0, PCIE_LTSSM_SII_TLB, x, PCIE_LOGICAL_Y,
						PCIE_SII_A_REG_MAP_BASE_ADDR);
#ifdef CONFIG_TT_BH_ARC_PCIE_LTSSM_LOG_CDR
				NOC2AXITlbSetup(0, PCIE_LTSSM_PHY0_TLB, x, PCIE_LOGICAL_Y,
						CMN_A_REG_MAP_BASE_ADDR);
				NOC2AXITlbSetup(0, PCIE_LTSSM_PHY1_TLB, x, PCIE_LOGICAL_Y,
						CMN_A_REG_MAP_BASE_ADDR + SERDES_INST_OFFSET);
#endif
				tlb_inst = inst;
			}

			PCIE_SII_LTSSM_STATE_reg_u ltssm_state;

			ltssm_state.val = NOC2AXIRead32(0, PCIE_LTSSM_SII_TLB,
							PCIE_SII_A_LTSSM_STATE_REG_OFFSET);

			uint8_t state = ltssm_state.f.smlh_ltssm_state_sync;
			/* rdlh_link_up can change while the LTSSM stays in L0 */
			uint8_t link = ltssm_state.f.smlh_link_up_sync |
				       (ltssm_state.f.rdlh_link_up_sync << 1);

#ifdef CONFIG_TT_BH_ARC_PCIE_LTSSM_LOG_CDR
			/* Nothing to lock to in Detect; else keep the last value between polls */
			uint32_t cdr = last_cdr[inst];

			if (LTSSM_DETECT_STATES & BIT64(state)) {
				cdr = 0;
			} else if (++poll_div[inst] >=
				   CONFIG_TT_BH_ARC_PCIE_LTSSM_LOG_CDR_POLL_DIV) {
				poll_div[inst] = 0;
				cdr = ReadCdrLock(lanes[inst]);
			}
#else
			/* Lane count 0: the host shows CDR as not sampled. */
			uint32_t cdr = 0;
#endif

			if (state == last_state[inst] && link == last_link[inst] &&
			    cdr == last_cdr[inst]) {
				continue;
			}
			last_state[inst] = state;
			last_link[inst] = link;
			last_cdr[inst] = cdr;

			ltssm_log_record(inst, state, ltssm_state.f.smlh_link_up_sync,
					 ltssm_state.f.rdlh_link_up_sync, cdr, LtssmTimestamp());
		}
	}
}

/* Generous: without HW stack protection an overflow would silently corrupt memory */
#define LTSSM_LOG_STACK_SIZE 1024

static K_THREAD_STACK_DEFINE(ltssm_log_stack, LTSSM_LOG_STACK_SIZE);
static struct k_thread ltssm_log_thread;
static uint8_t ltssm_log_inst_mask;
static uint8_t ltssm_log_lanes[2];

static void LtssmLogThread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	CaptureLtssmTraining(ltssm_log_inst_mask, ltssm_log_lanes);
}

static void StartLtssmLogThread(uint8_t inst_mask, const uint8_t *lanes)
{
	ltssm_log_inst_mask = inst_mask;
	ltssm_log_lanes[0] = MIN(lanes[0], LTSSM_CDR_MAX_LANES);
	ltssm_log_lanes[1] = MIN(lanes[1], LTSSM_CDR_MAX_LANES);

	/* Publish here, not in the thread, so scratch 24 is valid once pcie_init() returns */
	ltssm_log_reset();

	k_thread_create(&ltssm_log_thread, ltssm_log_stack, K_THREAD_STACK_SIZEOF(ltssm_log_stack),
			LtssmLogThread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0,
			K_NO_WAIT);
	k_thread_name_set(&ltssm_log_thread, "ltssm_log");
}
#endif /* CONFIG_TT_BH_ARC_PCIE_LTSSM_LOG */

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

	uint64_t board_id = bh_chip_info_board_id();
	uint32_t vendor_id = bh_chip_info_vendor_id();
	struct bh_pci_property pci0_property_table;
	struct bh_pci_property pci1_property_table;
	struct CntlInitV3Param param;

	bh_chip_info_pci_property(0, &pci0_property_table);
	bh_chip_info_pci_property(1, &pci1_property_table);

	if (pci0_property_table.pcie_mode != BH_PCIE_MODE_DISABLED) {
		CntlInitV3ParamInit(0, board_id, vendor_id, &pci0_property_table, &param);
		PCIeInit(&param);
	}

	if (pci1_property_table.pcie_mode != BH_PCIE_MODE_DISABLED) {
		CntlInitV3ParamInit(1, board_id, vendor_id, &pci1_property_table, &param);
		PCIeInit(&param);
	}

	InitResetInterrupt(0);
	InitResetInterrupt(1);

	WriteReg(PCIE_INIT_CPL_TIME_REG_ADDR, TimerTimestamp());

#ifdef CONFIG_TT_BH_ARC_PCIE_LTSSM_LOG
	/* After the completion timestamp, so recording does not inflate PCIe init time */
	uint8_t ltssm_inst_mask = 0;
	uint8_t ltssm_lanes[2] = {0, 0};

	if (pci0_property_table.pcie_mode != BH_PCIE_MODE_DISABLED) {
		ltssm_inst_mask |= BIT(0);
		ltssm_lanes[0] = pci0_property_table.num_serdes * PHY_LANES_PER_SERDES;
	}
	if (pci1_property_table.pcie_mode != BH_PCIE_MODE_DISABLED) {
		ltssm_inst_mask |= BIT(1);
		ltssm_lanes[1] = pci1_property_table.num_serdes * PHY_LANES_PER_SERDES;
	}

	if (ltssm_inst_mask != 0) {
		StartLtssmLogThread(ltssm_inst_mask, ltssm_lanes);
	}
#endif

	return 0;
}
SYS_INIT_APP(pcie_init);
