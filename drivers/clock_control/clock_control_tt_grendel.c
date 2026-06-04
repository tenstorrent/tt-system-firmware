/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_grendel_clock_control

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <platform.h>

LOG_MODULE_REGISTER(clock_control_tt_grendel, CONFIG_CLOCK_CONTROL_LOG_LEVEL);

#define CGM_READ(reg) sys_read32(cfg->base + SMC_CPU_SMC_WRAP_PLL_CNTL_CGM_0_##reg##_REG_OFFSET)
#define CGM_WRITE(reg, val)                                                                        \
	sys_write32((val), cfg->base + SMC_CPU_SMC_WRAP_PLL_CNTL_CGM_0_##reg##_REG_OFFSET)

#define POSTDIV_CONFIG_BYPASS_THEN_SWITCH 0x0
#define POSTDIV_CONFIG_ALWAYS_POSTDIV     0x1
#define POSTDIV_CONFIG_FORCE_BYPASS       0x2
#define POSTDIV_CONFIG_FORCE_GATE         0x3

#define FCW_INT_MIN   16
#define FCW_INT_MAX   255
#define FCW_FRAC_BITS 14

#define POSTDIV_MIN 1
#define POSTDIV_MAX 4

#define CGM_NUM_OUTPUTS 4

struct clock_control_tt_grendel_config {
	uintptr_t base;
	uint32_t ref_clk;
};

struct clock_control_tt_grendel_data {
	struct k_spinlock lock;
};

static inline void cgm_strobe_reg_update(const struct clock_control_tt_grendel_config *cfg)
{
	CGM_WRITE(REG_UPDATE, BIT(0));
}

static int cgm_wait_for_lock(const struct clock_control_tt_grendel_config *cfg)
{
	k_timepoint_t deadline =
		sys_timepoint_calc(K_MSEC(CONFIG_CLOCK_CONTROL_TT_GRENDEL_LOCK_TIMEOUT));

	do {
		if (CGM_READ(CGM_STATUS) & CGM_CGM_STATUS_LOCK_DETECT_MASK) {
			return 0;
		}
	} while (!K_TIMEOUT_EQ(sys_timepoint_timeout(deadline), K_NO_WAIT));

	return -ETIMEDOUT;
}

static uint32_t postdiv_mask_for_output(uint8_t output)
{
	switch (output) {
	case 0:
		return CGM_POSTDIV_ARRAY_0_POSTDIV0_MASK;
	case 1:
		return CGM_POSTDIV_ARRAY_0_POSTDIV1_MASK;
	case 2:
		return CGM_POSTDIV_ARRAY_0_POSTDIV2_MASK;
	case 3:
		return CGM_POSTDIV_ARRAY_0_POSTDIV3_MASK;
	default:
		return 0;
	}
}

static uint8_t cgm_get_postdiv(const struct clock_control_tt_grendel_config *cfg, uint8_t output)
{
	uint32_t reg = CGM_READ(POSTDIV_ARRAY_0);

	return FIELD_GET(postdiv_mask_for_output(output), reg);
}

static void cgm_set_postdiv(const struct clock_control_tt_grendel_config *cfg, uint8_t output,
			    uint8_t postdiv)
{
	uint32_t mask = postdiv_mask_for_output(output);
	uint32_t reg = CGM_READ(POSTDIV_ARRAY_0);

	reg &= ~mask;
	reg |= FIELD_PREP(mask, postdiv);
	CGM_WRITE(POSTDIV_ARRAY_0, reg);
}

static void cgm_set_postdiv_config(const struct clock_control_tt_grendel_config *cfg,
				   uint8_t output, uint8_t config_val)
{
	CGM_POSTDIV_CONFIG_reg_u reg = {.val = CGM_READ(POSTDIV_CONFIG)};

	switch (output) {
	case 0:
		reg.f.postdiv0_config = config_val;
		break;
	case 1:
		reg.f.postdiv1_config = config_val;
		break;
	case 2:
		reg.f.postdiv2_config = config_val;
		break;
	case 3:
		reg.f.postdiv3_config = config_val;
		break;
	default:
		return;
	}

	CGM_WRITE(POSTDIV_CONFIG, reg.val);
}

static uint8_t cgm_get_postdiv_config(const struct clock_control_tt_grendel_config *cfg,
				      uint8_t output)
{
	CGM_POSTDIV_CONFIG_reg_u reg = {.val = CGM_READ(POSTDIV_CONFIG)};

	switch (output) {
	case 0:
		return reg.f.postdiv0_config;
	case 1:
		return reg.f.postdiv1_config;
	case 2:
		return reg.f.postdiv2_config;
	case 3:
		return reg.f.postdiv3_config;
	default:
		return 0;
	}
}

static int clock_control_tt_grendel_on(const struct device *dev, clock_control_subsys_t sys)
{
	const struct clock_control_tt_grendel_config *cfg = dev->config;
	struct clock_control_tt_grendel_data *data = dev->data;
	uint8_t output = (uint8_t)(uintptr_t)sys;
	k_spinlock_key_t key;
	uint32_t cgm_enable;
	int ret;

	if (output >= CGM_NUM_OUTPUTS) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);

	cgm_enable = CGM_READ(ENABLES);

	if (!(cgm_enable & CGM_ENABLES_CGM_ENABLE_MASK)) {
		cgm_enable = CGM_ENABLES_CGM_ENABLE_MASK | CGM_ENABLES_FREQ_ACQ_ENABLE_MASK |
			     CGM_ENABLES_DROPOUT_ENABLE_MASK;
		CGM_WRITE(ENABLES, cgm_enable);
	}

	cgm_set_postdiv_config(cfg, output, POSTDIV_CONFIG_BYPASS_THEN_SWITCH);

	cgm_strobe_reg_update(cfg);

	ret = cgm_wait_for_lock(cfg);

	k_spin_unlock(&data->lock, key);
	return ret;
}

static int clock_control_tt_grendel_off(const struct device *dev, clock_control_subsys_t sys)
{
	const struct clock_control_tt_grendel_config *cfg = dev->config;
	struct clock_control_tt_grendel_data *data = dev->data;
	uint8_t output = (uint8_t)(uintptr_t)sys;
	k_spinlock_key_t key;

	if (output >= CGM_NUM_OUTPUTS) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);

	cgm_set_postdiv_config(cfg, output, POSTDIV_CONFIG_FORCE_GATE);
	cgm_strobe_reg_update(cfg);

	k_spin_unlock(&data->lock, key);
	return 0;
}

static int clock_control_tt_grendel_set_rate(const struct device *dev, clock_control_subsys_t sys,
					     clock_control_subsys_rate_t rate)
{
	const struct clock_control_tt_grendel_config *cfg = dev->config;
	struct clock_control_tt_grendel_data *data = dev->data;
	uint8_t output = (uint8_t)(uintptr_t)sys;
	uint32_t tgt_rate = (uint32_t)(uintptr_t)rate;
	k_spinlock_key_t key;
	uint8_t postdiv = 0;
	uint8_t fcw_int = 0;
	uint16_t fcw_frac = 0;
	uint32_t enables;
	bool found = false;
	int ret;

	if (output >= CGM_NUM_OUTPUTS) {
		return -EINVAL;
	}

	if (tgt_rate == 0 || cfg->ref_clk == 0) {
		return -EINVAL;
	}

	for (postdiv = POSTDIV_MIN; postdiv <= POSTDIV_MAX; postdiv++) {
		uint64_t fcw_fixed;

		fcw_fixed = ((uint64_t)tgt_rate << (postdiv + FCW_FRAC_BITS)) / cfg->ref_clk;
		fcw_int = (uint8_t)(fcw_fixed >> FCW_FRAC_BITS);
		fcw_frac = (uint16_t)(fcw_fixed & GENMASK(FCW_FRAC_BITS - 1, 0));

		if (fcw_int >= FCW_INT_MIN && fcw_int <= FCW_INT_MAX) {
			found = true;
			break;
		}
	}

	if (!found) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);

	enables = CGM_READ(ENABLES);
	enables &= ~CGM_ENABLES_FREQ_ACQ_ENABLE_MASK;
	CGM_WRITE(ENABLES, enables);

	CGM_WRITE(FCW_INT, fcw_int);
	CGM_WRITE(FCW_FRAC, fcw_frac);
	cgm_set_postdiv(cfg, output, postdiv);

	enables |= CGM_ENABLES_FREQ_ACQ_ENABLE_MASK;
	CGM_WRITE(ENABLES, enables);

	cgm_strobe_reg_update(cfg);

	ret = cgm_wait_for_lock(cfg);

	k_spin_unlock(&data->lock, key);
	return ret;
}

static int clock_control_tt_grendel_get_rate(const struct device *dev, clock_control_subsys_t sys,
					     uint32_t *rate)
{
	const struct clock_control_tt_grendel_config *cfg = dev->config;
	struct clock_control_tt_grendel_data *data = dev->data;
	uint8_t output = (uint8_t)(uintptr_t)sys;
	k_spinlock_key_t key;
	uint8_t postdiv_config;
	uint8_t postdiv;
	uint32_t fcw_int;
	uint32_t fcw_frac;
	uint64_t fcw_combined;
	int ret = 0;

	if (output >= CGM_NUM_OUTPUTS) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);

	postdiv_config = cgm_get_postdiv_config(cfg, output);

	if (postdiv_config == POSTDIV_CONFIG_FORCE_GATE) {
		ret = -EAGAIN;
	} else if (postdiv_config == POSTDIV_CONFIG_FORCE_BYPASS) {
		*rate = cfg->ref_clk;
	} else {
		fcw_int = CGM_READ(FCW_INT) & CGM_FCW_INT_FCW_INT_MASK;
		fcw_frac = CGM_READ(FCW_FRAC) & CGM_FCW_FRAC_FCW_FRAC_MASK;
		postdiv = cgm_get_postdiv(cfg, output);

		fcw_combined = ((uint64_t)fcw_int << FCW_FRAC_BITS) | fcw_frac;
		*rate = (uint32_t)(((uint64_t)cfg->ref_clk * fcw_combined) >>
				   (FCW_FRAC_BITS + postdiv));
	}

	k_spin_unlock(&data->lock, key);
	return ret;
}

static int clock_control_tt_grendel_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static DEVICE_API(clock_control, clock_control_tt_grendel_api) = {
	.on = clock_control_tt_grendel_on,
	.off = clock_control_tt_grendel_off,
	.get_rate = clock_control_tt_grendel_get_rate,
	.set_rate = clock_control_tt_grendel_set_rate,
};

#define CLOCK_CONTROL_TT_GRENDEL_INIT(_inst)                                                       \
	static struct clock_control_tt_grendel_data clock_control_tt_grendel_data_##_inst;         \
                                                                                                   \
	static const struct clock_control_tt_grendel_config                                        \
		clock_control_tt_grendel_config_##_inst = {                                        \
			.base = DT_REG_ADDR(DT_DRV_INST(_inst)),                                   \
			.ref_clk = DT_PROP(DT_INST_CLOCKS_CTLR(_inst), clock_frequency),           \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(_inst, clock_control_tt_grendel_init, NULL,                          \
			      &clock_control_tt_grendel_data_##_inst,                              \
			      &clock_control_tt_grendel_config_##_inst, POST_KERNEL,               \
			      CONFIG_CLOCK_CONTROL_INIT_PRIORITY, &clock_control_tt_grendel_api);

DT_INST_FOREACH_STATUS_OKAY(CLOCK_CONTROL_TT_GRENDEL_INIT)
