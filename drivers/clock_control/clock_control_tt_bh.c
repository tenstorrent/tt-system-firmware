/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_bh_clock_control

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/clock_control_tt_bh.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys_clock.h>
#include <zephyr/sys/util.h>
#include <stdint.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(clock_control_tt_bh);

#define PLL_LOCK_TIMEOUT_MS 400

#define PLL_CNTL_0_OFFSET             0x00
#define PLL_CNTL_1_OFFSET             0x04
#define PLL_CNTL_2_OFFSET             0x08
#define PLL_CNTL_3_OFFSET             0x0C
#define PLL_CNTL_4_OFFSET             0x10
#define PLL_CNTL_5_OFFSET             0x14
#define PLL_CNTL_6_OFFSET             0x18
#define PLL_USE_POSTDIV_OFFSET        0x1C
#define PLL_REFCLK_SEL_OFFSET         0x20
#define PLL_USE_FINE_DIVIDER_1_OFFSET 0x24
#define PLL_USE_FINE_DIVIDER_2_OFFSET 0x28
#define FINE_DUTYC_ADJUST_OFFSET      0x2C
#define CLK_COUNTER_EN_OFFSET         0x30
#define CLK_COUNTER_0_OFFSET          0x34
#define CLK_COUNTER_1_OFFSET          0x38
#define CLK_COUNTER_2_OFFSET          0x3C
#define CLK_COUNTER_3_OFFSET          0x40
#define CLK_COUNTER_4_OFFSET          0x44
#define CLK_COUNTER_5_OFFSET          0x48
#define CLK_COUNTER_6_OFFSET          0x4C
#define CLK_COUNTER_7_OFFSET          0x50

#define VCO_MIN_FREQ                            1600
#define VCO_MAX_FREQ                            5000
#define CLK_COUNTER_REFCLK_PERIOD               1000
#define PLL_CNTL_WRAPPER_PLL_LOCK_REG_ADDR      0x80020040
#define PLL_CNTL_WRAPPER_REFCLK_PERIOD_REG_ADDR 0x8002002C

struct tt_bh_pll_cntl_wrapper_lock_fields {
	uint32_t pll0_lock: 1;
	uint32_t pll1_lock: 1;
	uint32_t pll2_lock: 1;
	uint32_t pll3_lock: 1;
	uint32_t pll4_lock: 1;
};

union tt_bh_pll_cntl_wrapper_lock_reg {
	uint32_t val;
	struct tt_bh_pll_cntl_wrapper_lock_fields f;
};

struct tt_bh_pll_cntl_0_fields {
	uint32_t reset: 1;
	uint32_t pd: 1;
	uint32_t reset_lock: 1;
	uint32_t pd_bandgap: 1;
	uint32_t bypass: 1;
};

union tt_bh_pll_cntl_0_reg {
	uint32_t val;
	struct tt_bh_pll_cntl_0_fields f;
};

struct tt_bh_pll_cntl_1_fields {
	uint32_t refdiv: 8;
	uint32_t postdiv: 8;
	uint32_t fbdiv: 16;
};

union tt_bh_pll_cntl_1_reg {
	uint32_t val;
	struct tt_bh_pll_cntl_1_fields f;
};

struct tt_bh_pll_cntl_2_fields {
	uint32_t ctrl_bus1: 8;
	uint32_t ctrl_bus2: 8;
	uint32_t ctrl_bus3: 8;
	uint32_t ctrl_bus4: 8;
};

union tt_bh_pll_cntl_2_reg {
	uint32_t val;
	struct tt_bh_pll_cntl_2_fields f;
};

struct tt_bh_pll_cntl_3_fields {
	uint32_t ctrl_bus5: 8;
	uint32_t test_bus: 8;
	uint32_t lock_detect1: 16;
};

union tt_bh_pll_cntl_3_reg {
	uint32_t val;
	struct tt_bh_pll_cntl_3_fields f;
};

struct tt_bh_pll_cntl_5_fields {
	uint32_t postdiv0: 8;
	uint32_t postdiv1: 8;
	uint32_t postdiv2: 8;
	uint32_t postdiv3: 8;
};

union tt_bh_pll_cntl_5_reg {
	uint32_t val;
	struct tt_bh_pll_cntl_5_fields f;
};

struct tt_bh_pll_use_postdiv_fields {
	uint32_t pll_use_postdiv0: 1;
	uint32_t pll_use_postdiv1: 1;
	uint32_t pll_use_postdiv2: 1;
	uint32_t pll_use_postdiv3: 1;
	uint32_t pll_use_postdiv4: 1;
	uint32_t pll_use_postdiv5: 1;
	uint32_t pll_use_postdiv6: 1;
	uint32_t pll_use_postdiv7: 1;
};

union tt_bh_pll_use_postdiv_reg {
	uint32_t val;
	struct tt_bh_pll_use_postdiv_fields f;
};

struct tt_bh_pll_settings {
	union tt_bh_pll_cntl_1_reg pll_cntl_1;
	union tt_bh_pll_cntl_2_reg pll_cntl_2;
	union tt_bh_pll_cntl_3_reg pll_cntl_3;
	union tt_bh_pll_cntl_5_reg pll_cntl_5;
	union tt_bh_pll_use_postdiv_reg use_postdiv;
};

struct clock_control_tt_bh_config {
	uint8_t inst;

	uint32_t refclk_rate;

	uintptr_t base;
	size_t size;

	struct tt_bh_pll_settings init_settings;
};

struct clock_control_tt_bh_data {
	struct tt_bh_pll_settings settings;

	struct k_spinlock lock;
};

static uint32_t clock_control_tt_bh_read_reg(const struct clock_control_tt_bh_config *config,
					     uint32_t offset)
{
	__ASSERT(offset <= config->size,
		 "Register offset 0x%08x is not within PLL's size of 0x%08x bytes", offset,
		 config->size);

	return sys_read32(config->base + offset);
}

static void clock_control_tt_bh_write_reg(const struct clock_control_tt_bh_config *config,
					  uint32_t offset, uint32_t val)
{
	__ASSERT(offset <= config->size,
		 "Register offset 0x%08x is not within PLL's size of 0x%08x bytes", offset,
		 config->size);

	sys_write32(val, config->base + offset);
}

static void clock_control_enable_clk_counters(const struct clock_control_tt_bh_config *config)
{
	sys_write32(CLK_COUNTER_REFCLK_PERIOD, PLL_CNTL_WRAPPER_REFCLK_PERIOD_REG_ADDR);
	clock_control_tt_bh_write_reg(config, CLK_COUNTER_EN_OFFSET, 0xff);
}

static void clock_control_tt_bh_config_vco(const struct clock_control_tt_bh_config *config,
					   const struct tt_bh_pll_settings *settings)
{
	/* refdiv, postdiv, fbdiv */
	clock_control_tt_bh_write_reg(config, PLL_CNTL_1_OFFSET, settings->pll_cntl_1.val);
	/* FOUT4PHASEEN, FOUTPOSTDIVEN */
	clock_control_tt_bh_write_reg(config, PLL_CNTL_2_OFFSET, settings->pll_cntl_2.val);
	/* Disable SSCG */
	clock_control_tt_bh_write_reg(config, PLL_CNTL_3_OFFSET, settings->pll_cntl_3.val);
}

static void clock_control_tt_bh_config_ext_postdivs(const struct clock_control_tt_bh_config *config,
						    const struct tt_bh_pll_settings *settings)
{
	/* Disable postdivs before changing postdivs */
	clock_control_tt_bh_write_reg(config, PLL_USE_POSTDIV_OFFSET, 0x0);
	/* Set postdivs */
	clock_control_tt_bh_write_reg(config, PLL_CNTL_5_OFFSET, settings->pll_cntl_5.val);
	/* Enable postdivs */
	clock_control_tt_bh_write_reg(config, PLL_USE_POSTDIV_OFFSET, settings->use_postdiv.val);
}

static int clock_control_tt_bh_wait_lock(uint8_t inst)
{
	union tt_bh_pll_cntl_wrapper_lock_reg pll_lock_reg;
	uint64_t start = k_uptime_get();

	do {
		pll_lock_reg.val = sys_read32(PLL_CNTL_WRAPPER_PLL_LOCK_REG_ADDR);
		if (pll_lock_reg.val & BIT(inst)) {
			return 0;
		}
	} while (k_uptime_get() - start < PLL_LOCK_TIMEOUT_MS);

	LOG_ERR("PLL %d failed to lock within %d ms", inst, PLL_LOCK_TIMEOUT_MS);
	return -ETIMEDOUT;
}

static uint32_t clock_control_tt_bh_get_ext_postdiv(uint8_t postdiv_index,
						    union tt_bh_pll_cntl_5_reg pll_cntl_5,
						    union tt_bh_pll_use_postdiv_reg use_postdiv)
{
	uint32_t postdiv_value;
	bool postdiv_enabled;

	switch (postdiv_index) {
	case 0:
		postdiv_value = pll_cntl_5.f.postdiv0;
		postdiv_enabled = use_postdiv.f.pll_use_postdiv0;
		break;
	case 1:
		postdiv_value = pll_cntl_5.f.postdiv1;
		postdiv_enabled = use_postdiv.f.pll_use_postdiv1;
		break;
	case 2:
		postdiv_value = pll_cntl_5.f.postdiv2;
		postdiv_enabled = use_postdiv.f.pll_use_postdiv2;
		break;
	case 3:
		postdiv_value = pll_cntl_5.f.postdiv3;
		postdiv_enabled = use_postdiv.f.pll_use_postdiv3;
		break;
	default:
		__builtin_unreachable();
	}
	if (postdiv_enabled) {
		uint32_t eff_postdiv;

		if (postdiv_value == 0) {
			eff_postdiv = 0;
		} else if (postdiv_value <= 16) {
			eff_postdiv = postdiv_value + 1;
		} else {
			eff_postdiv = (postdiv_value + 1) * 2;
		}

		return eff_postdiv;
	} else {
		return 1;
	}
}

static uint32_t clock_control_tt_bh_calculate_fbdiv(uint32_t refclk_rate, uint32_t target_freq_mhz,
						    union tt_bh_pll_cntl_1_reg pll_cntl_1,
						    union tt_bh_pll_cntl_5_reg pll_cntl_5,
						    union tt_bh_pll_use_postdiv_reg use_postdiv,
						    uint8_t postdiv_index)
{
	uint32_t eff_postdiv =
		clock_control_tt_bh_get_ext_postdiv(postdiv_index, pll_cntl_5, use_postdiv);

	/* Means clock is disabled */
	if (eff_postdiv == 0) {
		return 0;
	}
	return target_freq_mhz * pll_cntl_1.f.refdiv * eff_postdiv / refclk_rate;
}

/* What we don't support: */
/* 1. PLL_CNTL_O.bypass */
/* 2. Internal bypass */
/* 3. Internal postdiv - PLL_CNTL_1.postdiv */
/* 4. Fractional feedback divider */
/* 5. Fine Divider */
static uint32_t clock_control_tt_bh_get_freq(const struct clock_control_tt_bh_config *config,
					     uint8_t postdiv_index)
{
	union tt_bh_pll_cntl_1_reg pll_cntl_1;
	union tt_bh_pll_cntl_5_reg pll_cntl_5;
	union tt_bh_pll_use_postdiv_reg use_postdiv;

	pll_cntl_1.val = clock_control_tt_bh_read_reg(config, PLL_CNTL_1_OFFSET);
	pll_cntl_5.val = clock_control_tt_bh_read_reg(config, PLL_CNTL_5_OFFSET);
	use_postdiv.val = clock_control_tt_bh_read_reg(config, PLL_USE_POSTDIV_OFFSET);

	uint32_t eff_postdiv =
		clock_control_tt_bh_get_ext_postdiv(postdiv_index, pll_cntl_5, use_postdiv);

	/* Clock is disabled */
	if (eff_postdiv == 0) {
		return 0;
	}

	return (config->refclk_rate * pll_cntl_1.f.fbdiv) / (pll_cntl_1.f.refdiv * eff_postdiv);
}

static void clock_control_tt_bh_update(const struct clock_control_tt_bh_config *config,
				       struct clock_control_tt_bh_data *data,
				       const struct tt_bh_pll_settings *settings)
{
	union tt_bh_pll_cntl_0_reg pll_cntl_0;

	/* Before turning off PLL, bypass PLL so glitch free mux has no chance to switch */
	pll_cntl_0.val = clock_control_tt_bh_read_reg(config, PLL_CNTL_0_OFFSET);
	pll_cntl_0.f.bypass = 0;

	clock_control_tt_bh_write_reg(config, PLL_CNTL_0_OFFSET, pll_cntl_0.val);

	k_busy_wait(3);

	/* Power down PLL and disable PLL reset */
	pll_cntl_0.val = 0;
	clock_control_tt_bh_write_reg(config, PLL_CNTL_0_OFFSET, pll_cntl_0.val);

	clock_control_tt_bh_config_vco(config, settings);

	/* Power sequence requires PLLEN get asserted 1us after all inputs are stable. */
	/* Wait 5x this time to be conservative */
	k_busy_wait(5);

	/* Power up PLLs */
	pll_cntl_0.f.pd = 1;
	clock_control_tt_bh_write_reg(config, PLL_CNTL_0_OFFSET, pll_cntl_0.val);

	/* Wait for PLLs to lock */
	clock_control_tt_bh_wait_lock(config->inst);

	/* Setup external postdivs */
	clock_control_tt_bh_config_ext_postdivs(config, settings);

	k_busy_wait_ns(300);

	/* Disable PLL bypass */
	pll_cntl_0.f.bypass = 1;
	clock_control_tt_bh_write_reg(config, PLL_CNTL_0_OFFSET, pll_cntl_0.val);

	k_busy_wait_ns(300);

	data->settings = *settings;
}

static int clock_control_tt_bh_enable(const struct device *dev, clock_control_subsys_t sys,
				      uint8_t enable)
{
	enum clock_control_tt_bh_clock clock = (enum clock_control_tt_bh_clock)(uintptr_t)sys;
	struct clock_control_tt_bh_config *config =
		(struct clock_control_tt_bh_config *)dev->config;
	struct clock_control_tt_bh_data *data = (struct clock_control_tt_bh_data *)dev->data;
	struct tt_bh_pll_settings settings = data->settings;

	switch (clock) {
	case CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_0:
		settings.pll_cntl_5.f.postdiv0 = enable;
		break;
	case CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_1:
		settings.pll_cntl_5.f.postdiv1 = enable;
		break;
	case CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_2:
		settings.pll_cntl_5.f.postdiv2 = enable;
		break;
	case CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_3:
		settings.pll_cntl_5.f.postdiv3 = enable;
		break;

	default:
		return -ENOSYS;
	}

	clock_control_tt_bh_update(config, data, &settings);
	return 0;
}
static int clock_control_tt_bh_on(const struct device *dev, clock_control_subsys_t sys)
{
	return clock_control_tt_bh_enable(dev, sys, 1U);
}

static int clock_control_tt_bh_off(const struct device *dev, clock_control_subsys_t sys)
{
	return clock_control_tt_bh_enable(dev, sys, 0U);
}

static int clock_control_tt_bh_async_on(const struct device *dev, clock_control_subsys_t sys,
					clock_control_cb_t cb, void *user_data)
{
	return -ENOSYS;
}

static int clock_control_tt_bh_get_rate(const struct device *dev, clock_control_subsys_t sys,
					uint32_t *rate)
{
	const struct clock_control_tt_bh_config *config =
		(const struct clock_control_tt_bh_config *)dev->config;
	struct clock_control_tt_bh_data *data = (struct clock_control_tt_bh_data *)dev->data;
	k_spinlock_key_t key;

	if (k_spin_trylock(&data->lock, &key) < 0) {
		return -EBUSY;
	}

	enum clock_control_tt_bh_clock clock = (enum clock_control_tt_bh_clock)(uintptr_t)sys;

	switch (clock) {
	case CLOCK_CONTROL_TT_BH_CLOCK_AICLK:
		*rate = clock_control_tt_bh_get_freq(config, 0);
		break;
	case CLOCK_CONTROL_TT_BH_CLOCK_ARCCLK:
		*rate = clock_control_tt_bh_get_freq(config, 0);
		break;
	case CLOCK_CONTROL_TT_BH_CLOCK_AXICLK:
		*rate = clock_control_tt_bh_get_freq(config, 1);
		break;
	case CLOCK_CONTROL_TT_BH_CLOCK_APBCLK:
		*rate = clock_control_tt_bh_get_freq(config, 2);
		break;
	case CLOCK_CONTROL_TT_BH_CLOCK_GDDRMEMCLK:
		*rate = clock_control_tt_bh_get_freq(config, 0);
		break;
	case CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_0:
		*rate = clock_control_tt_bh_get_freq(config, 0);
		break;
	case CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_1:
		*rate = clock_control_tt_bh_get_freq(config, 1);
		break;
	case CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_2:
		*rate = clock_control_tt_bh_get_freq(config, 2);
		break;
	case CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_3:
		*rate = clock_control_tt_bh_get_freq(config, 3);
		break;
	default:
		k_spin_unlock(&data->lock, key);
		return -ENOTSUP;
	}

	k_spin_unlock(&data->lock, key);
	return 0;
}

static enum clock_control_status clock_control_tt_bh_get_status(const struct device *dev,
								clock_control_subsys_t sys)
{
	return CLOCK_CONTROL_STATUS_UNKNOWN;
}

static int clock_control_tt_bh_set_rate(const struct device *dev, clock_control_subsys_t sys,
					clock_control_subsys_rate_t rate)
{
	const struct clock_control_tt_bh_config *config =
		(const struct clock_control_tt_bh_config *)dev->config;
	struct clock_control_tt_bh_data *data = (struct clock_control_tt_bh_data *)dev->data;
	k_spinlock_key_t key;

	if (k_spin_trylock(&data->lock, &key) < 0) {
		return -EBUSY;
	}

	enum clock_control_tt_bh_clock clock = (enum clock_control_tt_bh_clock)(uintptr_t)sys;

	if (clock == CLOCK_CONTROL_TT_BH_CLOCK_GDDRMEMCLK) {
		struct tt_bh_pll_settings settings = data->settings;
		uint32_t fbdiv = clock_control_tt_bh_calculate_fbdiv(
			config->refclk_rate, (uint32_t)rate, settings.pll_cntl_1,
			settings.pll_cntl_5, settings.use_postdiv, 0);
		if (fbdiv == 0) {
			k_spin_unlock(&data->lock, key);
			return -EINVAL;
		}
		settings.pll_cntl_1.f.fbdiv = fbdiv;
		uint32_t vco_freq = (config->refclk_rate * settings.pll_cntl_1.f.fbdiv) /
				    settings.pll_cntl_1.f.refdiv;

		if (!IN_RANGE(vco_freq, VCO_MIN_FREQ, VCO_MAX_FREQ)) {
			k_spin_unlock(&data->lock, key);
			return -ERANGE;
		}

		clock_control_tt_bh_update(config, data, &settings);
	} else if (clock == CLOCK_CONTROL_TT_BH_CLOCK_AICLK) {
		uint32_t target_fbdiv;
		union tt_bh_pll_cntl_1_reg pll_cntl_1;
		union tt_bh_pll_cntl_5_reg pll_cntl_5;
		union tt_bh_pll_use_postdiv_reg use_postdiv;

		pll_cntl_1.val = clock_control_tt_bh_read_reg(config, PLL_CNTL_1_OFFSET);
		pll_cntl_5.val = clock_control_tt_bh_read_reg(config, PLL_CNTL_5_OFFSET);
		use_postdiv.val = clock_control_tt_bh_read_reg(config, PLL_USE_POSTDIV_OFFSET);
		target_fbdiv =
			clock_control_tt_bh_calculate_fbdiv(config->refclk_rate, (uint32_t)rate,
							    pll_cntl_1, pll_cntl_5, use_postdiv, 0);

		while (pll_cntl_1.f.fbdiv != target_fbdiv) {
			if (target_fbdiv > pll_cntl_1.f.fbdiv) {
				pll_cntl_1.f.fbdiv += 1;
			} else {
				pll_cntl_1.f.fbdiv -= 1;
			}

			clock_control_tt_bh_write_reg(config, PLL_CNTL_1_OFFSET, pll_cntl_1.val);
			k_busy_wait_ns(100);
		}
	} else if (clock == CLOCK_CONTROL_TT_BH_INIT_STATE) {
		struct tt_bh_pll_settings settings = config->init_settings;

		clock_control_tt_bh_update(config, data, &settings);
		clock_control_enable_clk_counters(config);
	} else {
		k_spin_unlock(&data->lock, key);
		return -ENOTSUP;
	}

	k_spin_unlock(&data->lock, key);
	return 0;
}

static int clock_control_tt_bh_configure(const struct device *dev, clock_control_subsys_t sys,
					 void *option)
{
	const struct clock_control_tt_bh_config *config =
		(const struct clock_control_tt_bh_config *)dev->config;
	struct clock_control_tt_bh_data *data = (struct clock_control_tt_bh_data *)dev->data;
	k_spinlock_key_t key;

	if (k_spin_trylock(&data->lock, &key) < 0) {
		return -EBUSY;
	}

	if ((enum clock_control_tt_bh_clock_config)option == CLOCK_CONTROL_TT_BH_CONFIG_BYPASS) {
		/* No need to bypass refclk as it's not support */

		union tt_bh_pll_cntl_0_reg pll_cntl_0;

		/* Bypass PLL to refclk */
		pll_cntl_0.val = clock_control_tt_bh_read_reg(config, PLL_CNTL_0_OFFSET);
		pll_cntl_0.f.bypass = 0;

		clock_control_tt_bh_write_reg(config, PLL_CNTL_0_OFFSET, pll_cntl_0.val);

		k_busy_wait(3);

		/* Disable all external postdivs on all PLLs */
		clock_control_tt_bh_write_reg(config, PLL_USE_POSTDIV_OFFSET, 0);

		k_spin_unlock(&data->lock, key);
		return 0;
	}

	k_spin_unlock(&data->lock, key);
	return -ENOTSUP;
}

static int clock_control_tt_bh_init(const struct device *dev)
{
	const struct clock_control_tt_bh_config *config =
		(const struct clock_control_tt_bh_config *)dev->config;
	struct clock_control_tt_bh_data *data = (struct clock_control_tt_bh_data *)dev->data;
	k_spinlock_key_t key;
	int ret;

	if (k_spin_trylock(&data->lock, &key) < 0) {
		return -EBUSY;
	}

	data->settings = config->init_settings;
	union tt_bh_pll_cntl_0_reg pll_cntl_0;

	/* Before turning off PLL, bypass PLL so glitch free mux has no chance to switch */
	pll_cntl_0.val = clock_control_tt_bh_read_reg(config, PLL_CNTL_0_OFFSET);
	pll_cntl_0.f.bypass = 0;

	clock_control_tt_bh_write_reg(config, PLL_CNTL_0_OFFSET, pll_cntl_0.val);

	k_busy_wait(3);

	/* Power down PLL and disable PLL reset */
	pll_cntl_0.val = 0;
	clock_control_tt_bh_write_reg(config, PLL_CNTL_0_OFFSET, pll_cntl_0.val);

	clock_control_tt_bh_config_vco(config, &config->init_settings);

	/* Power sequence requires PLLEN get asserted 1us after all inputs are stable. */
	/* Wait 5x this time to be conservative */
	k_busy_wait(5);

	/* Power up PLLs */
	pll_cntl_0.f.pd = 1;
	clock_control_tt_bh_write_reg(config, PLL_CNTL_0_OFFSET, pll_cntl_0.val);

	/* Wait for PLLs to lock */
	ret = clock_control_tt_bh_wait_lock(config->inst);
	if (ret < 0) {
		return ret;
	}

	/* Setup external postdivs */
	clock_control_tt_bh_config_ext_postdivs(config, &config->init_settings);

	k_busy_wait_ns(300);

	/* Disable PLL bypass */
	pll_cntl_0.f.bypass = 1;
	clock_control_tt_bh_write_reg(config, PLL_CNTL_0_OFFSET, pll_cntl_0.val);

	k_busy_wait_ns(300);

	clock_control_enable_clk_counters(config);

	k_spin_unlock(&data->lock, key);
	return 0;
}

static DEVICE_API(clock_control,
		  clock_control_tt_bh_api) = {.on = clock_control_tt_bh_on,
					      .off = clock_control_tt_bh_off,
					      .async_on = clock_control_tt_bh_async_on,
					      .get_rate = clock_control_tt_bh_get_rate,
					      .get_status = clock_control_tt_bh_get_status,
					      .set_rate = clock_control_tt_bh_set_rate,
					      .configure = clock_control_tt_bh_configure};

#define CLOCK_CONTROL_TT_BH_INIT(_inst)                                                            \
	static struct clock_control_tt_bh_data clock_control_tt_bh_data_##_inst;                   \
                                                                                                   \
	static const struct clock_control_tt_bh_config clock_control_tt_bh_config_##_inst = {      \
		.inst = _inst,                                                                     \
		.refclk_rate = DT_PROP(DT_INST_CLOCKS_CTLR(_inst), clock_frequency),               \
		.base = DT_REG_ADDR(DT_DRV_INST(_inst)),                                           \
		.size = DT_REG_SIZE(DT_DRV_INST(_inst)),                                           \
		.init_settings = {                                                                 \
			.pll_cntl_1 = {.f.refdiv = DT_INST_PROP(_inst, refdiv),                    \
				       .f.postdiv = DT_INST_PROP(_inst, postdiv),                  \
				       .f.fbdiv = DT_INST_PROP(_inst, fbdiv)},                     \
			.pll_cntl_2 = {.f.ctrl_bus1 = DT_INST_PROP(_inst, ctrl_bus1)},             \
			.pll_cntl_3 = {.f.ctrl_bus5 = DT_INST_PROP(_inst, ctrl_bus5)},             \
			.pll_cntl_5 = {.f.postdiv0 = DT_INST_PROP_BY_IDX(_inst, post_divs, 0),     \
				       .f.postdiv1 = DT_INST_PROP_BY_IDX(_inst, post_divs, 1),     \
				       .f.postdiv2 = DT_INST_PROP_BY_IDX(_inst, post_divs, 2),     \
				       .f.postdiv3 = DT_INST_PROP_BY_IDX(_inst, post_divs, 3)},    \
			.use_postdiv = {.f.pll_use_postdiv0 =                                      \
						DT_INST_PROP_BY_IDX(_inst, use_post_divs, 0),      \
					.f.pll_use_postdiv1 =                                      \
						DT_INST_PROP_BY_IDX(_inst, use_post_divs, 1),      \
					.f.pll_use_postdiv2 =                                      \
						DT_INST_PROP_BY_IDX(_inst, use_post_divs, 2),      \
					.f.pll_use_postdiv3 =                                      \
						DT_INST_PROP_BY_IDX(_inst, use_post_divs, 3)}}};   \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(                                                                     \
		_inst, clock_control_tt_bh_init, NULL, &clock_control_tt_bh_data_##_inst,          \
		&clock_control_tt_bh_config_##_inst, POST_KERNEL, 3, &clock_control_tt_bh_api);

DT_INST_FOREACH_STATUS_OKAY(CLOCK_CONTROL_TT_BH_INIT)
