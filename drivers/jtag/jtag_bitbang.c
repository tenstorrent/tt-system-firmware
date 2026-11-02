/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "axi.h"
#include "jtag_profile_functions.h"
#include "tenstorrent/bitrev.h"

#include "jtag_priv.h"

#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/jtag.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#define DT_DRV_COMPAT zephyr_jtag_gpio

#define MST_TAP_OP_ISCAN_SEL (2)
#define MST_TAP_OP_DEVID_SEL (6)
#define TENSIX_SM_RTAP       (0x19e)
#define TENSIX_SM_SIBLEN     (4)
#define TENSIX_SM_TDRLEN     (32)

#define TENSIX_SIBLEN_PLUS_1_OR_0   ((TENSIX_SM_SIBLEN > 0) ? (TENSIX_SM_SIBLEN + 1) : 0)
#define TENSIX_TDRLEN_SIBLEN_PLUS_1 (1 + TENSIX_SM_TDRLEN + TENSIX_SM_SIBLEN)
#define SIBSHIFT(x)                 ((x) >> TENSIX_SM_SIBLEN)
#define SIBSHIFTUP(x)               ((x) << TENSIX_SM_SIBLEN)

#define INSTR_REG_BISTEN_SEL_END_0   (9)
#define INSTR_REG_BISTEN_SEL_START_0 (7)
#define INSTR_REG_BISTEN_SEL_MASK_0  (7)
#define INSTR_REG_BISTEN_SEL_MASK_1  (0x3f)

typedef struct {
	uint32_t op: 3;
	uint32_t rsvd0: 4;
	uint32_t bisten_sel_0: 3;
	uint32_t rsvd1: 7;
	uint32_t bisten_sel_1: 6;
	uint32_t rsvd2: 9;
} jtag_instr_t;

typedef union {
	jtag_instr_t f;
	uint32_t val;
} jtag_instr_u;

#ifdef CONFIG_JTAG_PROFILE_FUNCTIONS
static uint32_t io_ops;
#endif

LOG_MODULE_REGISTER(jtag_bitbang, CONFIG_JTAG_LOG_LEVEL);

/*
 * Protects the shared JTAG GPIOs. setup() acquires it and teardown() releases it, so a full
 * session is held by one thread; setup/teardown must be called from the same thread. Individual
 * operations also lock it (k_mutex is recursive), which keeps standalone calls safe.
 */
static K_MUTEX_DEFINE(jtag_mutex);

#ifdef CONFIG_JTAG_USE_MMAPPED_IO

#define TCK_BSSR(config) (config->tck_reg + 6)
#define TDI_BSSR(config) (config->tdi_reg + 6)
#define TDO_BSSR(config) (config->tdo_reg + 6)
#define TMS_BSSR(config) (config->tms_reg + 6)

#define TDO_IN(config) (config->tdo_reg + 4)

#define TCK_HIGH(config) (1 << config->tck.pin)
#define TCK_LOW(config)  (1 << (config->tck.pin + 16))
#define SET_TCK(config)  (*TCK_BSSR(config) = TCK_HIGH(config))
#define CLR_TCK(config)  (*TCK_BSSR(config) = TCK_LOW(config))

#define TDI_HIGH(dev)        (1 << config->tdi.pin)
#define TDI_LOW(config)      (1 << (config->tdi.pin + 16))
#define SET_TDI(config)      (*TDI_BSSR(config) = TDI_HIGH(config))
#define CLR_TDI(config)      (*TDI_BSSR(config) = TDI_LOW(config))
#define IF_TDI(config, stmt) (*TDI_BSSR(config) = stmt ? TDI_HIGH(config) : TDI_LOW(config))

#define TDO_MSK(config) (1 << config->tdo.pin)
#define GET_TDO(config) ((*TDO_IN(config) & TDO_MSK(config)) != 0)

#define TMS_HIGH(config) (1 << config->tms.pin)
#define TMS_LOW(config)  (1 << (config->tms.pin + 16))
#define SET_TMS(config)  (*TMS_BSSR(config) = TMS_HIGH(config))
#define CLR_TMS(config)  (*TMS_BSSR(config) = TMS_LOW(config))

#else /* CONFIG_JTAG_USE_MMAPPED_IO */

#ifdef CONFIG_JTAG_PROFILE_FUNCTIONS

#define SET_TCK(x) IO_OPS_INC()
#define CLR_TCK(x) IO_OPS_INC()

#define SET_TDI(x) IO_OPS_INC()
#define CLR_CLR(x) IO_OPS_INC()

static bool GET_TDO(const struct jtag_config *config)
{
	IO_OPS_INC();
	return true;
}

#define SET_TMS(x) IO_OPS_INC()
#define CLR_TMS(x) IO_OPS_INC()

#else

static void SET_TCK(const struct jtag_config *config)
{
	gpio_pin_set_dt(&config->tck, 1);
}
static void CLR_TCK(const struct jtag_config *config)
{
	gpio_pin_set_dt(&config->tck, 0);
}

static void SET_TDI(const struct jtag_config *config)
{
	gpio_pin_set_dt(&config->tdi, 1);
}
static void CLR_TDI(const struct jtag_config *config)
{
	gpio_pin_set_dt(&config->tdi, 0);
}

static bool GET_TDO(const struct jtag_config *config)
{
	return gpio_pin_get_dt(&config->tdo);
}

static void SET_TMS(const struct jtag_config *config)
{
	gpio_pin_set_dt(&config->tms, 1);
}
static void CLR_TMS(const struct jtag_config *config)
{
	gpio_pin_set_dt(&config->tms, 0);
}

#endif /* CONFIG_JTAG_PROFILE_FUNCTIONS */

#define IF_TDI(config, stmt)                                                                       \
	do {                                                                                       \
		if (stmt) {                                                                        \
			SET_TDI(config);                                                           \
		} else {                                                                           \
			CLR_TDI(config);                                                           \
		}                                                                                  \
	} while (0)

#endif /* CONFIG_JTAG_USE_MMAPPED_IO */

static ALWAYS_INLINE void jtag_bitbang_tick(const struct device *dev, uint32_t count)
{
	const struct jtag_config *config = dev->config;

	for (; count > 0; --count) {
		CLR_TCK(config);
		SET_TCK(config);
	}
	CLR_TCK(config);
}

int jtag_bitbang_reset(const struct device *dev)
{
	k_mutex_lock(&jtag_mutex, K_FOREVER);

	const struct jtag_config *config = dev->config;

	if (config->trst.port != NULL) {
		gpio_pin_set_dt(&config->trst, 1);
		k_busy_wait(100);
		gpio_pin_set_dt(&config->trst, 0);
	}
	CLR_TDI(config);
	SET_TMS(config);

	jtag_bitbang_tick(dev, 5);

	CLR_TMS(config);

	jtag_bitbang_tick(dev, 1);

	k_mutex_unlock(&jtag_mutex);

	return 0;
}

static ALWAYS_INLINE void jtag_bitbang_update_ir(const struct device *dev, uint32_t count,
						 uint64_t data)
{
	const struct jtag_config *config = dev->config;

	/* Select IR scan */
	SET_TMS(config);
	jtag_bitbang_tick(dev, 2);

	/* Capture IR */
	CLR_TMS(config);
	jtag_bitbang_tick(dev, 1);

	/* Shift IR */
	for (; count > 1; --count, data >>= 1) {
		IF_TDI(config, data & 0x1);
		jtag_bitbang_tick(dev, 1);
	}

	/* Exit IR */
	SET_TMS(config);
	IF_TDI(config, data & 0x1);
	jtag_bitbang_tick(dev, 1);

	/* Select DR scan */
	SET_TMS(config);
	jtag_bitbang_tick(dev, 2);
}

static ALWAYS_INLINE uint64_t jtag_bitbang_xfer_dr(const struct device *dev, uint32_t count,
						   uint64_t data_in, bool idle, bool capture)
{
	if (count == 0) {
		return 0;
	}

	const struct jtag_config *config = dev->config;

	uint64_t starting_count = count;
	uint64_t data_out = 0;

	/* DR Scan */
	CLR_TMS(config);
	jtag_bitbang_tick(dev, 1);

	/* Capture DR */
	jtag_bitbang_tick(dev, 1);

	/* Shift DR */
	for (; count > 1; --count, data_in >>= 1) {
		IF_TDI(config, data_in & 0x1);
		if (capture) {
			data_out |= GET_TDO(config);
			data_out <<= 1;
		}
		jtag_bitbang_tick(dev, 1);
	}
	SET_TMS(config);
	IF_TDI(config, data_in & 0x1);
	if (capture) {
		data_out |= GET_TDO(config);
	}
	jtag_bitbang_tick(dev, 1);

	/* Exit DR */
	jtag_bitbang_tick(dev, 1);

	if (idle) {
		/* Update DR */
		CLR_TMS(config);
		jtag_bitbang_tick(dev, 1);

		/* Idle */
	} else {
		/* Update DR */
		jtag_bitbang_tick(dev, 1);

		/* DR scan */
	}

	if (capture) {
		data_out <<= 64 - starting_count;
		data_out = bitrev64(data_out);
	}

	return data_out;
}

static ALWAYS_INLINE uint64_t jtag_bitbang_capture_dr_idle(const struct device *dev, uint32_t count,
							   uint64_t data_in)
{
	return jtag_bitbang_xfer_dr(dev, count, data_in, true, true);
}

static ALWAYS_INLINE uint64_t jtag_bitbang_capture_dr(const struct device *dev, uint32_t count,
						      uint64_t data_in)
{
	return jtag_bitbang_xfer_dr(dev, count, data_in, false, true);
}

static ALWAYS_INLINE void jtag_bitbang_update_dr_idle(const struct device *dev, uint32_t count,
						      uint64_t data_in)
{
	(void)jtag_bitbang_xfer_dr(dev, count, data_in, true, false);
}

static ALWAYS_INLINE void jtag_bitbang_update_dr(const struct device *dev, uint32_t count,
						 uint64_t data_in)
{
	(void)jtag_bitbang_xfer_dr(dev, count, data_in, false, false);
}

int jtag_bitbang_read_id(const struct device *dev, uint32_t *id)
{
	k_mutex_lock(&jtag_mutex, K_FOREVER);

	uint32_t tap_addr = 6;

	jtag_bitbang_update_ir(dev, 24, tap_addr);
	*id = jtag_bitbang_capture_dr_idle(dev, 32, 0);

	k_mutex_unlock(&jtag_mutex);

	return 0;
}

int jtag_bitbang_setup(const struct device *dev)
{
	k_mutex_lock(&jtag_mutex, K_FOREVER);

	const struct jtag_config *config = dev->config;
	int ret = gpio_pin_configure_dt(&config->tck, GPIO_OUTPUT_ACTIVE) ||
		  gpio_pin_configure_dt(&config->tdi, GPIO_OUTPUT_ACTIVE) ||
		  gpio_pin_configure_dt(&config->tdo, GPIO_INPUT) ||
		  gpio_pin_configure_dt(&config->tms, GPIO_OUTPUT_ACTIVE);

	if (!ret && config->trst.port != NULL) {
		ret = gpio_pin_configure_dt(&config->trst, GPIO_OUTPUT_ACTIVE);
	}
	if (ret) {
		k_mutex_unlock(&jtag_mutex);
		return ret;
	}

#ifdef CONFIG_JTAG_USE_MMAPPED_IO
	volatile uint32_t *TCK_SPEED = (volatile uint32_t *)config->tck_reg + 2;
	volatile uint32_t *TDI_SPEED = (volatile uint32_t *)config->tdi_reg + 2;
	volatile uint32_t *TDO_SPEED = (volatile uint32_t *)config->tdo_reg + 2;
	volatile uint32_t *TMS_SPEED = (volatile uint32_t *)config->tms_reg + 2;

	*TCK_SPEED = *TCK_SPEED | (0b11 << (config->tck.pin * 2));
	*TDI_SPEED = *TDI_SPEED | (0b11 << (config->tdi.pin * 2));
	*TDO_SPEED = *TDO_SPEED | (0b11 << (config->tdo.pin * 2));
	*TMS_SPEED = *TMS_SPEED | (0b11 << (config->tms.pin * 2));
#endif /* CONFIG_JTAG_USE_MMAPPED_IO */

	return 0;
}

static int jtag_bitbang_teardown(const struct device *dev)
{
	const struct jtag_config *config = dev->config;
	int ret = gpio_pin_configure_dt(&config->tck, GPIO_INPUT) ||
		  gpio_pin_configure_dt(&config->tdi, GPIO_INPUT) ||
		  gpio_pin_configure_dt(&config->tdo, GPIO_INPUT) ||
		  gpio_pin_configure_dt(&config->tms, GPIO_INPUT);
	if (!ret && config->trst.port != NULL) {
		ret = gpio_pin_configure_dt(&config->trst, GPIO_INPUT);
	}
	if (ret) {
		k_mutex_unlock(&jtag_mutex);
		return ret;
	}

#ifdef CONFIG_JTAG_USE_MMAPPED_IO
	volatile uint32_t *TCK_SPEED = (volatile uint32_t *)config->tck_reg + 2;
	volatile uint32_t *TDI_SPEED = (volatile uint32_t *)config->tdi_reg + 2;
	volatile uint32_t *TDO_SPEED = (volatile uint32_t *)config->tdo_reg + 2;
	volatile uint32_t *TMS_SPEED = (volatile uint32_t *)config->tms_reg + 2;

	*TCK_SPEED = *TCK_SPEED | (0b00 << (config->tck.pin * 2));
	*TDI_SPEED = *TDI_SPEED | (0b00 << (config->tdi.pin * 2));
	*TDO_SPEED = *TDO_SPEED | (0b00 << (config->tdo.pin * 2));
	*TMS_SPEED = *TMS_SPEED | (0b00 << (config->tms.pin * 2));
#endif /* CONFIG_JTAG_USE_MMAPPED_IO */

	k_mutex_unlock(&jtag_mutex);
	return 0;
}

static ALWAYS_INLINE void jtag_setup_access(const struct device *dev, uint32_t rtap_addr)
{
	jtag_instr_u instrn;

	instrn.val = 0;
	instrn.f.op = MST_TAP_OP_ISCAN_SEL;
	instrn.f.bisten_sel_0 = rtap_addr & INSTR_REG_BISTEN_SEL_MASK_0;
	instrn.f.bisten_sel_1 =
		(rtap_addr >> (INSTR_REG_BISTEN_SEL_END_0 - INSTR_REG_BISTEN_SEL_START_0 + 1)) &
		INSTR_REG_BISTEN_SEL_MASK_1;

	jtag_bitbang_update_ir(dev, 24, instrn.val);
}

static ALWAYS_INLINE uint32_t jtag_access_rtap_tdr_idle(const struct device *dev,
							uint32_t rtap_addr, uint32_t tdr_addr,
							uint32_t wrdata)
{
	jtag_bitbang_update_dr(dev, TENSIX_SIBLEN_PLUS_1_OR_0, (uint64_t)tdr_addr + 1);
	return SIBSHIFT(jtag_bitbang_capture_dr_idle(dev, TENSIX_TDRLEN_SIBLEN_PLUS_1,
						     SIBSHIFTUP((uint64_t)wrdata)));
}

static ALWAYS_INLINE uint32_t jtag_access_rtap_tdr(const struct device *dev, uint32_t rtap_addr,
						   uint32_t tdr_addr, uint32_t wrdata)
{
	jtag_bitbang_update_dr(dev, TENSIX_SIBLEN_PLUS_1_OR_0, (uint64_t)tdr_addr + 1);
	return SIBSHIFT(jtag_bitbang_capture_dr(dev, TENSIX_TDRLEN_SIBLEN_PLUS_1,
						SIBSHIFTUP((uint64_t)wrdata)));
}

static ALWAYS_INLINE void jtag_wr_tensix_sm_rtap_tdr_idle(const struct device *dev,
							  uint32_t tdr_addr, uint32_t wrvalue)
{
	jtag_bitbang_update_dr(dev, TENSIX_SIBLEN_PLUS_1_OR_0, (uint64_t)tdr_addr + 1);
	jtag_bitbang_update_dr_idle(dev, TENSIX_TDRLEN_SIBLEN_PLUS_1,
				    SIBSHIFTUP((uint64_t)wrvalue));
}

static ALWAYS_INLINE void jtag_wr_tensix_sm_rtap_tdr(const struct device *dev, uint32_t tdr_addr,
						     uint32_t wrvalue)
{
	jtag_bitbang_update_dr(dev, TENSIX_SIBLEN_PLUS_1_OR_0, (uint64_t)tdr_addr + 1);
	jtag_bitbang_update_dr(dev, TENSIX_TDRLEN_SIBLEN_PLUS_1, SIBSHIFTUP((uint64_t)wrvalue));
}

static ALWAYS_INLINE uint32_t jtag_rd_tensix_sm_rtap_tdr_idle(const struct device *dev,
							      uint32_t tdr_addr)
{
	jtag_bitbang_update_dr(dev, TENSIX_SIBLEN_PLUS_1_OR_0, (uint64_t)tdr_addr + 1);
	return SIBSHIFT(jtag_bitbang_capture_dr_idle(dev, TENSIX_TDRLEN_SIBLEN_PLUS_1, 0));
}

static ALWAYS_INLINE uint32_t jtag_rd_tensix_sm_rtap_tdr(const struct device *dev,
							 uint32_t tdr_addr)
{
	jtag_bitbang_update_dr(dev, TENSIX_SIBLEN_PLUS_1_OR_0, (uint64_t)tdr_addr + 1);
	return SIBSHIFT(jtag_bitbang_capture_dr(dev, TENSIX_TDRLEN_SIBLEN_PLUS_1, 0));
}

void jtag_req_clear(const struct device *dev)
{
	k_mutex_lock(&jtag_mutex, K_FOREVER);

	jtag_setup_access(dev, TENSIX_SM_RTAP);

	jtag_wr_tensix_sm_rtap_tdr_idle(dev, 2, AXI_CNTL_CLEAR);

	k_mutex_unlock(&jtag_mutex);
}

int jtag_axiread(const struct device *dev, uint32_t addr, uint32_t *result)
{
	k_mutex_lock(&jtag_mutex, K_FOREVER);

	jtag_setup_access(dev, TENSIX_SM_RTAP);

	jtag_wr_tensix_sm_rtap_tdr(dev, ARC_AXI_ADDR_TDR, addr);

	jtag_wr_tensix_sm_rtap_tdr(dev, ARC_AXI_CONTROL_STATUS_TDR, AXI_CNTL_READ);

	uint32_t axi_status = 1;

	for (int i = 0; i < 1000; ++i) {
		axi_status = jtag_rd_tensix_sm_rtap_tdr(dev, ARC_AXI_CONTROL_STATUS_TDR);
		if ((axi_status & 0xF) != 0) {
			break;
		}
	}

	/* Read data */
	uint32_t axi_rddata = jtag_rd_tensix_sm_rtap_tdr_idle(dev, ARC_AXI_DATA_TDR);

	*result = axi_rddata;

	k_mutex_unlock(&jtag_mutex);

	return (axi_status & 0xF) != 0 ? 0 : -1;
}

int jtag_axiwrite(const struct device *dev, uint32_t addr, uint32_t value)
{
	k_mutex_lock(&jtag_mutex, K_FOREVER);

	jtag_setup_access(dev, TENSIX_SM_RTAP);

	jtag_wr_tensix_sm_rtap_tdr(dev, ARC_AXI_ADDR_TDR, addr);
	jtag_wr_tensix_sm_rtap_tdr(dev, ARC_AXI_DATA_TDR, value);

	jtag_wr_tensix_sm_rtap_tdr(dev, ARC_AXI_CONTROL_STATUS_TDR, AXI_CNTL_WRITE);

	/* Upper 16 bits contain write status; if first bit 1 then we passed otherwsie */
	/* fail */
	int result =
		((jtag_rd_tensix_sm_rtap_tdr_idle(dev, ARC_AXI_CONTROL_STATUS_TDR) >> 16) & 1) != 1
			? 0
			: -1;

	k_mutex_unlock(&jtag_mutex);

	return result;
}

int jtag_axi_blockwrite(const struct device *dev, uint32_t addr, const uint32_t *value,
			uint32_t len)
{
	int result = 0;

	k_mutex_lock(&jtag_mutex, K_FOREVER);

	CYCLES_ENTRY();
	for (int i = 0; i < len; ++i) {
		/* Call the internal write without re-locking */
		jtag_setup_access(dev, TENSIX_SM_RTAP);
		jtag_wr_tensix_sm_rtap_tdr(dev, ARC_AXI_ADDR_TDR, addr + (4 * i));
		jtag_wr_tensix_sm_rtap_tdr(dev, ARC_AXI_DATA_TDR, value[i]);
		jtag_wr_tensix_sm_rtap_tdr(dev, ARC_AXI_CONTROL_STATUS_TDR, AXI_CNTL_WRITE);

		if (((jtag_rd_tensix_sm_rtap_tdr_idle(dev, ARC_AXI_CONTROL_STATUS_TDR) >> 16) &
		     1) == 1) {
			result |= -1;
		}
	}
	CYCLES_EXIT();

	k_mutex_unlock(&jtag_mutex);

	return result;
}

static struct jtag_api jtag_bitbang_api = {.setup = jtag_bitbang_setup,
					   .teardown = jtag_bitbang_teardown,
					   .read_id = jtag_bitbang_read_id,
					   .reset = jtag_bitbang_reset,
					   .axi_read32 = jtag_axiread,
					   .axi_write32 = jtag_axiwrite,
					   .axi_block_write = jtag_axi_blockwrite};

static int jtag_bitbang_init(const struct device *dev)
{
	return 0;
}

#define JTAG_BB_GPIOS_GET_REG(n, gpios)                                                            \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, gpios),                           \
		(INT_TO_POINTER(DT_REG_ADDR(DT_PHANDLE(DT_DRV_INST(n), gpios)))), (NULL))

#define JTAG_BB_DEVICE_DEFINE(n)                                                                   \
	static const struct jtag_config jtag_bitbang_config_##n = {                                \
		.tck = GPIO_DT_SPEC_INST_GET(n, tck_gpios),                                        \
		.tdi = GPIO_DT_SPEC_INST_GET(n, tdi_gpios),                                        \
		.tdo = GPIO_DT_SPEC_INST_GET(n, tdo_gpios),                                        \
		.tms = GPIO_DT_SPEC_INST_GET(n, tms_gpios),                                        \
		.trst = GPIO_DT_SPEC_INST_GET_OR(n, trst_gpios, {0}),                              \
		COND_CODE_1(CONFIG_JTAG_USE_MMAPPED_IO,                        \
		(.tck_reg = JTAG_BB_GPIOS_GET_REG(n, tck_gpios),               \
		 .tdi_reg = JTAG_BB_GPIOS_GET_REG(n, tdi_gpios),               \
		 .tdo_reg = JTAG_BB_GPIOS_GET_REG(n, tdo_gpios),               \
		 .tms_reg = JTAG_BB_GPIOS_GET_REG(n, tms_gpios),               \
		 .port_write_cycles = DT_INST_PROP(n, port_write_cycles),      \
		 ),                    \
		())};                                 \
                                                                                                   \
	static struct jtag_data jtag_bitbang_data_##n;                                             \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, jtag_bitbang_init, NULL, &jtag_bitbang_data_##n,                  \
			      &jtag_bitbang_config_##n, POST_KERNEL, CONFIG_JTAG_INIT_PRIO,        \
			      &jtag_bitbang_api);

DT_INST_FOREACH_STATUS_OKAY(JTAG_BB_DEVICE_DEFINE)
