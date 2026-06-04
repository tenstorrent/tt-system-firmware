/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_bh_pvt

#include "tenstorrent/smc_msg.h"
#include "tenstorrent/msgqueue.h"
#include "tenstorrent/post_code.h"
#include "functional_efuse.h"

#include <float.h> /* for FLT_MAX */
#include <math.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/clock_control_tt_bh.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/tenstorrent/pvt_tt_bh.h>

LOG_MODULE_REGISTER(pvt_tt_bh, LOG_LEVEL_DBG);

static const struct device *const pll_dev_1 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(pll1));

#define PVT_ID_NUM                           0x80080008
#define PVT_TM_SCRATCH                       0x8008000C
#define PVT_CNTL_IRQ_EN_REG_ADDR             0x80080040
#define PVT_CNTL_TS_00_IRQ_ENABLE_REG_ADDR   0x800800C0
#define PVT_CNTL_PD_00_IRQ_ENABLE_REG_ADDR   0x80080340
#define PVT_CNTL_VM_00_IRQ_ENABLE_REG_ADDR   0x80080A00
#define PVT_CNTL_TS_00_ALARMA_CFG_REG_ADDR   0x800800E0
#define PVT_CNTL_TS_00_ALARMB_CFG_REG_ADDR   0x800800E4
#define PVT_CNTL_TS_CMN_CLK_SYNTH_REG_ADDR   0x80080080
#define PVT_CNTL_PD_CMN_CLK_SYNTH_REG_ADDR   0x80080300
#define PVT_CNTL_VM_CMN_CLK_SYNTH_REG_ADDR   0x80080800
#define PVT_CNTL_PD_CMN_SDIF_STATUS_REG_ADDR 0x80080308
#define PVT_CNTL_PD_CMN_SDIF_REG_ADDR        0x8008030C
#define PVT_CNTL_TS_CMN_SDIF_STATUS_REG_ADDR 0x80080088
#define PVT_CNTL_TS_CMN_SDIF_REG_ADDR        0x8008008C
#define PVT_CNTL_PD_CMN_SDIF_STATUS_REG_ADDR 0x80080308
#define PVT_CNTL_PD_CMN_SDIF_REG_ADDR        0x8008030C
#define PVT_CNTL_VM_CMN_SDIF_STATUS_REG_ADDR 0x80080808
#define PVT_CNTL_VM_CMN_SDIF_REG_ADDR        0x8008080C
#define PVT_CNTL_TS_00_SDIF_DONE_REG_ADDR    0x800800D4
#define PVT_CNTL_TS_00_SDIF_DATA_REG_ADDR    0x800800D8
#define PVT_CNTL_VM_00_SDIF_RDATA_REG_ADDR   0x80080A30
#define PVT_CNTL_PD_00_SDIF_DONE_REG_ADDR    0x80080354
#define PVT_CNTL_PD_00_SDIF_DATA_REG_ADDR    0x80080358

/* these macros are used for registers specific for each sensor */
#define TS_PD_OFFSET                  0x40
#define VM_OFFSET                     0x200
#define GET_TS_REG_ADDR(ID, REG_NAME) (ID * TS_PD_OFFSET + PVT_CNTL_TS_00_##REG_NAME##_REG_ADDR)
#define GET_PD_REG_ADDR(ID, REG_NAME) (ID * TS_PD_OFFSET + PVT_CNTL_PD_00_##REG_NAME##_REG_ADDR)
#define GET_VM_REG_ADDR(ID, REG_NAME) (ID * VM_OFFSET + PVT_CNTL_VM_00_##REG_NAME##_REG_ADDR)

/* SDIF address */
#define IP_CNTL_ADDR    0x0
#define IP_CFG0_ADDR    0x1
#define IP_CFGA_ADDR    0x2
#define IP_DATA_ADDR    0x3
#define IP_POLLING_ADDR 0x4
#define IP_TMR_ADDR     0x5
#define IP_CFG1_ADDR    0x6

/* therm_trip temperature in degrees C */
#define ALARM_A_THERM_TRIP_TEMP 83
/* BH prod spec 7.3 gives Tj,shutdown=110C, tmons are +-1C calibrated */
#define ALARM_B_THERM_TRIP_TEMP 109

#define TS_HYSTERESIS_DELTA 5

#define ALL_AGING_OSC 0x7 /* enable delay chain 19, 20, 21 for aging measurement */

#define NUM_TS 8
#define NUM_VM 8
#define NUM_PD 16

typedef struct {
	uint32_t run_mode: 4;
	uint32_t reserved_1: 4;
	uint32_t oscillator_select: 5;
	uint32_t oscillator_enable: 3;
	uint32_t counter_divide_ratio: 2;
	uint32_t reserved_2: 2;
	uint32_t counter_gate: 2;
	uint32_t reserved_3: 10;
} pd_ip_cfg0_t;

typedef union {
	uint32_t val;
	pd_ip_cfg0_t f;
} pd_ip_cfg0_u;

typedef struct {
	uint32_t run_mode: 4;
	uint32_t reserved_0: 1;
	uint32_t resolution: 2;
	uint32_t reserved_1: 25;
} ts_ip_cfg0_t;

typedef union {
	uint8_t val;
	ts_ip_cfg0_t f;
} ts_ip_cfg0_u;

typedef struct {
	uint32_t tmr_irq_enable: 1;
	uint32_t ts_irq_enable: 1;
	uint32_t vm_irq_enable: 1;
	uint32_t pd_irq_enable: 1;
} pvt_cntl_irq_en_reg_t;

typedef union {
	uint32_t val;
	pvt_cntl_irq_en_reg_t f;
} pvt_cntl_irq_en_reg_u;

#define PVT_CNTL_IRQ_EN_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t irq_en_fault: 1;
	uint32_t irq_en_done: 1;
	uint32_t rsvd_0: 1;
	uint32_t irq_en_alarm_a: 1;
	uint32_t irq_en_alarm_b: 1;
} pvt_cntl_ts_pd_irq_enable_reg_t;

typedef union {
	uint32_t val;
	pvt_cntl_ts_pd_irq_enable_reg_t f;
} pvt_cntl_ts_pd_irq_enable_reg_u;

#define PVT_CNTL_TS_PD_IRQ_ENABLE_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t irq_en_fault: 1;
	uint32_t irq_en_done: 1;
} pvt_cntl_vm_irq_enable_reg_t;

typedef union {
	uint32_t val;
	pvt_cntl_vm_irq_enable_reg_t f;
} pvt_cntl_vm_irq_enable_reg_u;

#define PVT_CNTL_VM_IRQ_ENABLE_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t hyst_thresh: 16;
	uint32_t alarm_thresh: 16;
} pvt_cntl_vm_alarma_cfg_reg_t;

typedef union {
	uint32_t val;
	pvt_cntl_vm_alarma_cfg_reg_t f;
} pvt_cntl_vm_alarma_cfg_reg_u;

#define PVT_CNTL_VM_ALARMA_CFG_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t hyst_thresh: 16;
	uint32_t alarm_thresh: 16;
} pvt_cntl_vm_alarmb_cfg_reg_t;

typedef union {
	uint32_t val;
	pvt_cntl_vm_alarmb_cfg_reg_t f;
} pvt_cntl_vm_alarmb_cfg_reg_u;

#define PVT_CNTL_VM_ALARMB_CFG_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t clk_synth_lo: 8;
	uint32_t clk_synth_hi: 8;
	uint32_t clk_synth_hold: 4;
	uint32_t rsvd_0: 4;
	uint32_t clk_synth_en: 1;
} pvt_cntl_clk_synth_reg_t;

typedef union {
	uint32_t val;
	pvt_cntl_clk_synth_reg_t f;
} pvt_cntl_clk_synth_reg_u;

#define PVT_CNTL_CLK_SYNTH_REG_DEFAULT (0x00010000)

typedef struct {
	uint32_t sdif_busy: 1;
	uint32_t sdif_lock: 1;
} pvt_cntl_sdif_status_reg_t;

typedef union {
	uint32_t val;
	pvt_cntl_sdif_status_reg_t f;
} pvt_cntl_sdif_status_reg_u;

#define PVT_CNTL_SDIF_STATUS_REG_DEFAULT (0x00000000)

typedef struct {
	uint32_t sdif_wdata: 24;
	uint32_t sdif_addr: 3;
	uint32_t sdif_wrn: 1;
	uint32_t rsvd_0: 3;
	uint32_t sdif_prog: 1;
} pvt_cntl_sdif_reg_t;

typedef union {
	uint32_t val;
	pvt_cntl_sdif_reg_t f;
} pvt_cntl_sdif_reg_u;

#define PVT_CNTL_SDIF_REG_DEFAULT (0x00000000)

static uint16_t temp_to_dout(float temp)
{
	return (uint16_t)(((temp - 83.09f) / 262.5f + 0.5f) * 4096);
}

/* setup 4 sources of interrupts for each type of sensor: */
/* 1. sample done */
/* 2. alarm a: rising alarm, ignored (see section 14 of PVT controller spec) */
/* 3. alarm b: rising alarm (see section 14 of PVT controller spec) */
/* 4. IP has a fault */
/* For VM, only enable sample done and fault interrupts, as alarma and alarmb is per channel */
/* and we do not enable any channel in VM. */
static inline void pvt_tt_bh_interrupt_config(void)
{
	/* Enable Global interrupt for TS and PD */
	pvt_cntl_irq_en_reg_u irq_en;

	irq_en.val = PVT_CNTL_IRQ_EN_REG_DEFAULT;
	irq_en.f.ts_irq_enable = 1;
	irq_en.f.pd_irq_enable = 1;
	irq_en.f.vm_irq_enable = 1;
	sys_write32(irq_en.val, PVT_CNTL_IRQ_EN_REG_ADDR);

	/* Enable sources of interrupts for TS, PD, and VM */
	pvt_cntl_ts_pd_irq_enable_reg_u ts_irq_en;

	ts_irq_en.val = PVT_CNTL_TS_PD_IRQ_ENABLE_REG_DEFAULT;
	ts_irq_en.f.irq_en_alarm_a = 1;
	ts_irq_en.f.irq_en_alarm_b = 1;
	ts_irq_en.f.irq_en_done = 1;
	ts_irq_en.f.irq_en_fault = 1;
	for (uint32_t i = 0; i < NUM_TS; i++) {
		sys_write32(ts_irq_en.val, GET_TS_REG_ADDR(i, IRQ_ENABLE));
	}

	pvt_cntl_vm_irq_enable_reg_u pd_vm_irq_en;

	pd_vm_irq_en.val = PVT_CNTL_VM_IRQ_ENABLE_REG_DEFAULT;
	pd_vm_irq_en.f.irq_en_fault = 1;
	pd_vm_irq_en.f.irq_en_done = 1;
	for (uint32_t i = 0; i < NUM_PD; i++) {
		sys_write32(pd_vm_irq_en.val, GET_PD_REG_ADDR(i, IRQ_ENABLE));
	}

	for (uint32_t i = 0; i < NUM_VM; i++) {
		sys_write32(pd_vm_irq_en.val, GET_VM_REG_ADDR(i, IRQ_ENABLE));
	}

	/* Configure Alarm A */
	pvt_cntl_vm_alarma_cfg_reg_u pvt_alarma_cfg;

	pvt_alarma_cfg.val = PVT_CNTL_VM_ALARMA_CFG_REG_DEFAULT;
	pvt_alarma_cfg.f.hyst_thresh = temp_to_dout(ALARM_A_THERM_TRIP_TEMP - TS_HYSTERESIS_DELTA);
	pvt_alarma_cfg.f.alarm_thresh = temp_to_dout(ALARM_A_THERM_TRIP_TEMP);
	for (uint32_t i = 0; i < NUM_TS; i++) {
		sys_write32(pvt_alarma_cfg.val, GET_TS_REG_ADDR(i, ALARMA_CFG));
	}

	/* Configure Alarm B */
	pvt_cntl_vm_alarmb_cfg_reg_u pvt_alarmb_cfg;

	pvt_alarmb_cfg.val = PVT_CNTL_VM_ALARMB_CFG_REG_DEFAULT;
	pvt_alarmb_cfg.f.hyst_thresh = temp_to_dout(ALARM_B_THERM_TRIP_TEMP - TS_HYSTERESIS_DELTA);
	pvt_alarmb_cfg.f.alarm_thresh = temp_to_dout(ALARM_B_THERM_TRIP_TEMP);
	for (uint32_t i = 0; i < NUM_TS; i++) {
		sys_write32(pvt_alarmb_cfg.val, GET_TS_REG_ADDR(i, ALARMB_CFG));
	}
}

/* PVT clocks work in range of 4-8MHz and are derived from APB clock */
/* target a PVT clock of 8 MHz */
static inline void pvt_tt_bh_clock_config(void)
{
	uint32_t apb_clk = 0;

	clock_control_get_rate(pll_dev_1, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_APBCLK,
			       &apb_clk);
	pvt_cntl_clk_synth_reg_u clk_synt;

	clk_synt.val = PVT_CNTL_CLK_SYNTH_REG_DEFAULT;

	/* PVT clock is defined by # of APB cycles that it is high/low. We keep the high & low
	 * counts equal for a 50-50 duty cycle.
	 * So we want smallest count such that APB/2count <= target.
	 * APB/2target <= count, so count = ceil(APB/2target).
	 * For our settings of APB=100MHz, PVT target=8MHz, we get 100MHz/14=7.14MHz.
	 */
	const uint32_t target_clock_MHz = 8;
	uint32_t half_cycle = DIV_ROUND_UP(apb_clk, 2 * target_clock_MHz);

	clk_synt.f.clk_synth_lo = half_cycle - 1;
	clk_synt.f.clk_synth_hi = half_cycle - 1;
	clk_synt.f.clk_synth_hold = 2;
	clk_synt.f.clk_synth_en = 1;
	sys_write32(clk_synt.val, PVT_CNTL_TS_CMN_CLK_SYNTH_REG_ADDR);
	sys_write32(clk_synt.val, PVT_CNTL_PD_CMN_CLK_SYNTH_REG_ADDR);
	sys_write32(clk_synt.val, PVT_CNTL_VM_CMN_CLK_SYNTH_REG_ADDR);
}

static void wait_sdif_ready(uint32_t status_reg_addr)
{
	pvt_cntl_sdif_status_reg_u sdif_status;

	do {
		sdif_status.val = sys_read32(status_reg_addr);
	} while (sdif_status.f.sdif_busy == 1);
}

static void sdif_write(uint32_t status_reg_addr, uint32_t wr_data_reg_addr, uint32_t sdif_addr,
		       uint32_t data)
{
	wait_sdif_ready(status_reg_addr);
	pvt_cntl_sdif_reg_u sdif;

	sdif.val = PVT_CNTL_SDIF_REG_DEFAULT;
	sdif.f.sdif_addr = sdif_addr;
	sdif.f.sdif_wdata = data;
	sdif.f.sdif_wrn = 1;
	sdif.f.sdif_prog = 1;
	sys_write32(sdif.val, wr_data_reg_addr);
}

static void enable_aging_meas(void)
{
	pd_ip_cfg0_u ip_cfg0;

	ip_cfg0.val = 0;
	ip_cfg0.f.oscillator_enable = ALL_AGING_OSC;
	sdif_write(PVT_CNTL_PD_CMN_SDIF_STATUS_REG_ADDR, PVT_CNTL_PD_CMN_SDIF_REG_ADDR,
		   IP_CFG0_ADDR, ip_cfg0.val);
}

int pvt_tt_bh_attr_get(const struct device *dev, enum sensor_channel chan,
		       enum sensor_attribute attr, struct sensor_value *val)
{
	if (!dev || !val) {
		return -EINVAL;
	}

	const struct pvt_tt_bh_config *config = (const struct pvt_tt_bh_config *)dev->config;
	enum pvt_tt_bh_attribute pvt_attr = (enum pvt_tt_bh_attribute)attr;

	switch (pvt_attr) {
	case SENSOR_ATTR_PVT_TT_BH_NUM_PD:
		val->val1 = config->num_pd;
		break;
	case SENSOR_ATTR_PVT_TT_BH_NUM_VM:
		val->val1 = config->num_vm;
		break;
	case SENSOR_ATTR_PVT_TT_BH_NUM_TS:
		val->val1 = config->num_ts;
		break;
	default:
		return -ENOTSUP;
	}

	/* val2 is the fractional part, which is 0 for integers */
	val->val2 = 0;

	return 0;
}

/**
 * @brief Verifies if the PVT device is alive according to section 18.1 of the datasheet.
 *
 * Performs the following steps in order,
 *   1) Verifies ID is 0
 *   2) Verifies scratch register is 0x0
 *   3) Verifies writing scratch register by walking 1s
 *
 * If these checks fail, the PVT sensor should not be considered reliable.
 *
 * @retval 0	On success.
 * @retval -EIO	On hardware failure with error logs.
 */
static int pvt_tt_bh_is_alive(void)
{
	uint32_t id;
	uint32_t scratch;

	/* We don't set the ID, so verify it is 0 */
	id = sys_read32(PVT_ID_NUM);
	if (id != 0) {
		LOG_ERR("ID is %d, expected 0", id);
		return -EIO;
	}

	/* Verify scratch register is initially 0x0 */
	scratch = sys_read32(PVT_TM_SCRATCH);
	if (scratch != 0) {
		LOG_ERR("Scratch register is %x, expected 0x0", scratch);
		return -EIO;
	}

	/* Verify writing to the scratch register by walking 1s */
	for (int i = 0; i < 32; ++i) {
		sys_write32(BIT(i), PVT_TM_SCRATCH);
		if (sys_read32(PVT_TM_SCRATCH) != BIT(i)) {
			LOG_ERR("Writing to scratch register failed at bit %d", i);
			return -EIO;
		}
	}

	return 0;
}

/*
 * Setup Interrupt and clk configurations, TS, PD, VM IP configurations.
 * Enable continuous mode for TS and VM. For PD, run once mode should be used.
 */
static int pvt_tt_bh_init(const struct device *dev)
{
	const struct pvt_tt_bh_config *pvt_cfg = (const struct pvt_tt_bh_config *)dev->config;
	int ret;

	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEP5);

	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY) || !IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	ret = pvt_tt_bh_is_alive();
	if (ret != 0) {
		return ret;
	}

	/* Enable Process + Voltage + Thermal monitors */
	pvt_tt_bh_interrupt_config();
	pvt_tt_bh_clock_config();

	/* Configure TS */
	sdif_write(PVT_CNTL_TS_CMN_SDIF_STATUS_REG_ADDR, PVT_CNTL_TS_CMN_SDIF_REG_ADDR, IP_TMR_ADDR,
		   0x100); /* 256 cycles for TS */

	/* MODE_RUN_0, 8-bit resolution */
	ts_ip_cfg0_u ts_ip_cfg0 = {.f.run_mode = 0, .f.resolution = 2};

	sdif_write(PVT_CNTL_TS_CMN_SDIF_STATUS_REG_ADDR, PVT_CNTL_TS_CMN_SDIF_REG_ADDR,
		   IP_CFG0_ADDR, ts_ip_cfg0.val);
	sdif_write(PVT_CNTL_TS_CMN_SDIF_STATUS_REG_ADDR, PVT_CNTL_TS_CMN_SDIF_REG_ADDR,
		   IP_CNTL_ADDR, 0x108); /* ip_run_cont */

	/* Configure PD */
	sdif_write(PVT_CNTL_PD_CMN_SDIF_STATUS_REG_ADDR, PVT_CNTL_PD_CMN_SDIF_REG_ADDR, IP_TMR_ADDR,
		   0x0); /* 0 cycles for PD */
	sdif_write(PVT_CNTL_PD_CMN_SDIF_STATUS_REG_ADDR, PVT_CNTL_PD_CMN_SDIF_REG_ADDR,
		   IP_CNTL_ADDR, 0x100); /* ip_auto to release reset and pd */
	enable_aging_meas();

	/* Configure VM */
	sdif_write(PVT_CNTL_VM_CMN_SDIF_STATUS_REG_ADDR, PVT_CNTL_VM_CMN_SDIF_REG_ADDR, IP_TMR_ADDR,
		   0x40); /* 64 cycles for VM */
	sdif_write(PVT_CNTL_VM_CMN_SDIF_STATUS_REG_ADDR, PVT_CNTL_VM_CMN_SDIF_REG_ADDR,
		   IP_CFG0_ADDR,
		   0x1000); /* use 14-bit resolution, MODE_RUN_0, select supply check */
	sdif_write(PVT_CNTL_VM_CMN_SDIF_STATUS_REG_ADDR, PVT_CNTL_VM_CMN_SDIF_REG_ADDR,
		   IP_CNTL_ADDR, 0x108); /* ip_auto to release reset and pd */

	/* Wait for all sensors to power up, TS takes 256 ip_clk cycles */
	k_usleep(100);

	/* Initialize the single thermal calibration fuse value from TS0's 25 degree data */

	uint16_t celcius25_raw = pvt_tt_bh_temp_to_raw(25.0f);

	for (uint8_t id = 0; id < pvt_cfg->num_ts; ++id) {
		uint32_t deg25_start = 2240 + (id * 64);
		uint32_t deg25_end = deg25_start + 15; /* data is 16 bits */

		uint16_t efuse = (uint16_t)ReadFunctionalEfuse(deg25_start, deg25_end);
		float efuse_celcius = pvt_tt_bh_raw_to_temp(efuse);

		/* Only use the calibration value if it is no more than three degrees away from 25
		 */
		if (efuse_celcius >= 22.0f && efuse_celcius <= 28.0f) {
			pvt_cfg->therm_cali_delta[id] =
				(uint16_t)ReadFunctionalEfuse(deg25_start, deg25_end) -
				celcius25_raw;
		}
	}

	return 0;
}

static DEVICE_API(sensor, pvt_tt_bh_driver_api) = {
	.attr_set = NULL,
	.attr_get = pvt_tt_bh_attr_get,
	.trigger_set = NULL,

	/* Not implemented due to newer read (submit) and decode API preferred. */
	.sample_fetch = NULL,
	.channel_get = NULL,

	.submit = pvt_tt_bh_submit,
	.get_decoder = pvt_tt_bh_get_decoder,
};

#define DEFINE_PVT_TT_BH(id)                                                                       \
	static int16_t pvt_tt_bh_therm_cali_delta[DT_PROP(DT_DRV_INST(id), num_ts)] = {};          \
                                                                                                   \
	static const struct pvt_tt_bh_config pvt_tt_bh_config_##_id = {                            \
		.num_ts = DT_PROP(DT_DRV_INST(id), num_ts),                                        \
		.num_pd = DT_PROP(DT_DRV_INST(id), num_pd),                                        \
		.num_vm = DT_PROP(DT_DRV_INST(id), num_vm),                                        \
                                                                                                   \
		.therm_cali_delta = pvt_tt_bh_therm_cali_delta,                                    \
	};                                                                                         \
                                                                                                   \
	static struct pvt_tt_bh_data pvt_tt_bh_data_##_id = {};                                    \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(id, pvt_tt_bh_init, NULL, &pvt_tt_bh_data_##_id,                     \
			      &pvt_tt_bh_config_##_id, POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,   \
			      &pvt_tt_bh_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DEFINE_PVT_TT_BH)
