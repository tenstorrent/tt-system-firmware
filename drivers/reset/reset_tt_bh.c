/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_bh_reset

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/sys_io.h>

struct tt_bh_reset_config {
	uintptr_t base;
	uint32_t reset_mask;
};

struct tt_bh_reset_data {
	struct k_spinlock lock;
};

static inline bool tt_bh_reset_is_valid_id(const struct device *dev, uint32_t id)
{
	const struct tt_bh_reset_config *config = dev->config;

	if (id >= 32) {
		return false;
	}

	return (BIT(id) & config->reset_mask) != 0;
}

static int tt_bh_reset_status(const struct device *dev, uint32_t id, uint8_t *status)
{
	const struct tt_bh_reset_config *config = dev->config;

	if (!tt_bh_reset_is_valid_id(dev, id)) {
		return -EINVAL;
	}

	/* return the number of active-low reset lines that are asserted */
	*status = POPCOUNT(~sys_read32(config->base));

	return 0;
}

static int tt_bh_reset_line_assert(const struct device *dev, uint32_t id)
{
	const struct tt_bh_reset_config *config = dev->config;
	struct tt_bh_reset_data *const data = dev->data;
	uint32_t value;

	if (!tt_bh_reset_is_valid_id(dev, id)) {
		return -EINVAL;
	}

	K_SPINLOCK(&data->lock) {
		value = sys_read32(config->base);
		value &= ~BIT(id);
		sys_write32(value, config->base);
	}

	return 0;
}

static int tt_bh_reset_line_deassert(const struct device *dev, uint32_t id)
{
	const struct tt_bh_reset_config *config = dev->config;
	struct tt_bh_reset_data *const data = dev->data;
	uint32_t value;

	if (!tt_bh_reset_is_valid_id(dev, id)) {
		return -EINVAL;
	}

	K_SPINLOCK(&data->lock) {
		value = sys_read32(config->base);
		value |= BIT(id);
		sys_write32(value, config->base);
	}

	return 0;
}

static int tt_bh_reset_line_toggle(const struct device *dev, uint32_t id)
{
	const struct tt_bh_reset_config *config = dev->config;
	struct tt_bh_reset_data *const data = dev->data;
	uint32_t value;

	if (!tt_bh_reset_is_valid_id(dev, id)) {
		return -EINVAL;
	}

	K_SPINLOCK(&data->lock) {
		value = sys_read32(config->base);
		value &= ~BIT(id);
		sys_write32(value, config->base);
		value |= BIT(id);
		sys_write32(value, config->base);
	}

	return 0;
}

static int tt_bh_reset_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static DEVICE_API(reset, tt_bh_reset_api) = {
	.status = tt_bh_reset_status,
	.line_assert = tt_bh_reset_line_assert,
	.line_deassert = tt_bh_reset_line_deassert,
	.line_toggle = tt_bh_reset_line_toggle,
};

#define TT_BH_RESET_MASK_FROM_NRESETS(_n) ((uint32_t)BIT64_MASK(DT_INST_PROP_OR(_n, nresets, 0)))
#define TT_BH_RESET_MASK(_n)              DT_INST_PROP_OR(_n, reset_mask, 0)
#define TT_BH_NUM_RESET_SPECIFIERS(_n)                                                             \
	(!!DT_INST_PROP_OR(_n, nresets, 0) + !!DT_INST_PROP_OR(_n, reset_mask, 0))

#define TT_BH_RESET_MASK_DEFINE(_n)                                                                \
	((TT_BH_RESET_MASK(_n) | TT_BH_RESET_MASK_FROM_NRESETS(_n)) || (-1))

#define TT_BH_RESET_DEFINE(_n)                                                                     \
	BUILD_ASSERT(TT_BH_NUM_RESET_SPECIFIERS(_n) <= 1,                                          \
		     "Maximally 1 of nresets or reset-mask may be specified");                     \
                                                                                                   \
	static struct tt_bh_reset_data tt_bh_reset_data_##_n;                                      \
	static const struct tt_bh_reset_config tt_bh_reset_config_##_n = {                         \
		.base = DT_INST_REG_ADDR(_n),                                                      \
		.reset_mask = TT_BH_RESET_MASK_DEFINE(_n),                                         \
	};                                                                                         \
                                                                                                   \
	BUILD_ASSERT(TT_BH_RESET_MASK_DEFINE(_n) != 0, "reset mask should never be zero");         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(_n, tt_bh_reset_init, NULL, &tt_bh_reset_data_##_n,                  \
			      &tt_bh_reset_config_##_n, PRE_KERNEL_1, CONFIG_RESET_INIT_PRIORITY,  \
			      &tt_bh_reset_api);

DT_INST_FOREACH_STATUS_OKAY(TT_BH_RESET_DEFINE);
