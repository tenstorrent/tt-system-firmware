/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_bh_reset_emul

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/reset/reset_tt_bh.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

struct tt_bh_reset_emul_config {
	uint32_t reset_mask;
	uint32_t size;
};

struct tt_bh_reset_emul_data {
	struct k_spinlock lock;
	uint32_t *values;
};

static inline bool tt_bh_reset_emul_is_valid_id(const struct device *dev, uint32_t id)
{
	const struct tt_bh_reset_emul_config *config = dev->config;

	if (id >= 32) {
		return false;
	}

	return (BIT(id) & config->reset_mask) != 0;
}

static inline bool tt_bh_reset_emul_is_valid_offset(const struct device *dev, uint32_t offset)
{
	const struct tt_bh_reset_emul_config *config = dev->config;

	return (offset < config->size) && ((offset % TT_BH_RESET_BANK_STRIDE) == 0);
}

static inline uint32_t *tt_bh_reset_emul_bank(struct tt_bh_reset_emul_data *data, uint32_t offset)
{
	return &data->values[offset / TT_BH_RESET_BANK_STRIDE];
}

static int tt_bh_reset_emul_status(const struct device *dev, uint32_t id, uint8_t *status)
{
	struct tt_bh_reset_emul_data *const data = dev->data;

	if (!tt_bh_reset_emul_is_valid_id(dev, id)) {
		return -EINVAL;
	}

	/* Active-low: reset is asserted when the bit is clear (bank 0) */
	*status = !(data->values[0] & BIT(id));

	return 0;
}

static int tt_bh_reset_emul_line_assert(const struct device *dev, uint32_t id)
{
	struct tt_bh_reset_emul_data *const data = dev->data;

	if (!tt_bh_reset_emul_is_valid_id(dev, id)) {
		return -EINVAL;
	}

	K_SPINLOCK(&data->lock) {
		data->values[0] &= ~BIT(id);
	}

	return 0;
}

static int tt_bh_reset_emul_line_deassert(const struct device *dev, uint32_t id)
{
	struct tt_bh_reset_emul_data *const data = dev->data;

	if (!tt_bh_reset_emul_is_valid_id(dev, id)) {
		return -EINVAL;
	}

	K_SPINLOCK(&data->lock) {
		data->values[0] |= BIT(id);
	}

	return 0;
}

static int tt_bh_reset_emul_line_toggle(const struct device *dev, uint32_t id)
{
	struct tt_bh_reset_emul_data *const data = dev->data;

	if (!tt_bh_reset_emul_is_valid_id(dev, id)) {
		return -EINVAL;
	}

	K_SPINLOCK(&data->lock) {
		data->values[0] &= ~BIT(id);
		data->values[0] |= BIT(id);
	}

	return 0;
}

int reset_tt_bh_write(const struct device *dev, uint32_t value)
{
	struct tt_bh_reset_emul_data *const data = dev->data;

	K_SPINLOCK(&data->lock) {
		data->values[0] = value;
	}

	return 0;
}

int reset_tt_bh_lines_assert(const struct device *dev, uint32_t offset, uint32_t mask)
{
	const struct tt_bh_reset_emul_config *config = dev->config;
	struct tt_bh_reset_emul_data *const data = dev->data;
	uint32_t lines = mask & config->reset_mask;
	uint32_t *bank;

	if (!tt_bh_reset_emul_is_valid_offset(dev, offset) || lines == 0) {
		return -EINVAL;
	}

	bank = tt_bh_reset_emul_bank(data, offset);

	K_SPINLOCK(&data->lock) {
		*bank &= ~lines;
	}

	return 0;
}

int reset_tt_bh_lines_deassert(const struct device *dev, uint32_t offset, uint32_t mask)
{
	const struct tt_bh_reset_emul_config *config = dev->config;
	struct tt_bh_reset_emul_data *const data = dev->data;
	uint32_t lines = mask & config->reset_mask;
	uint32_t *bank;

	if (!tt_bh_reset_emul_is_valid_offset(dev, offset) || lines == 0) {
		return -EINVAL;
	}

	bank = tt_bh_reset_emul_bank(data, offset);

	K_SPINLOCK(&data->lock) {
		*bank |= lines;
	}

	return 0;
}

static int tt_bh_reset_emul_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static DEVICE_API(reset, tt_bh_reset_emul_api) = {
	.status = tt_bh_reset_emul_status,
	.line_assert = tt_bh_reset_emul_line_assert,
	.line_deassert = tt_bh_reset_emul_line_deassert,
	.line_toggle = tt_bh_reset_emul_line_toggle,
};

#define TT_BH_RESET_EMUL_MASK_FROM_NRESETS(_n)                                                     \
	((uint32_t)BIT64_MASK(DT_INST_PROP_OR(_n, nresets, 0)))
#define TT_BH_RESET_EMUL_MASK(_n) DT_INST_PROP_OR(_n, reset_mask, 0)
#define TT_BH_RESET_EMUL_NUM_SPECIFIERS(_n)                                                        \
	(!!DT_INST_PROP_OR(_n, nresets, 0) + !!DT_INST_PROP_OR(_n, reset_mask, 0))
#define TT_BH_RESET_EMUL_N_BANKS(_n) DT_INST_PROP_OR(_n, n_banks, 1)

#define TT_BH_RESET_EMUL_MASK_DEFINE(_n)                                                           \
	((TT_BH_RESET_EMUL_MASK(_n) | TT_BH_RESET_EMUL_MASK_FROM_NRESETS(_n))                      \
		 ? (TT_BH_RESET_EMUL_MASK(_n) | TT_BH_RESET_EMUL_MASK_FROM_NRESETS(_n))            \
		 : UINT32_MAX)

#define TT_BH_RESET_EMUL_DEFINE(_n)                                                                \
	BUILD_ASSERT(TT_BH_RESET_EMUL_NUM_SPECIFIERS(_n) <= 1,                                     \
		     "Maximally 1 of nresets or reset-mask may be specified");                     \
	BUILD_ASSERT(TT_BH_RESET_EMUL_N_BANKS(_n) >= 1, "n-banks must be at least 1");             \
                                                                                                   \
	static uint32_t tt_bh_reset_emul_vals_##_n[TT_BH_RESET_EMUL_N_BANKS(_n)];                  \
	static struct tt_bh_reset_emul_data tt_bh_reset_emul_data_##_n = {                         \
		.values = tt_bh_reset_emul_vals_##_n,                                              \
	};                                                                                         \
	static const struct tt_bh_reset_emul_config tt_bh_reset_emul_config_##_n = {               \
		.reset_mask = TT_BH_RESET_EMUL_MASK_DEFINE(_n),                                    \
		.size = TT_BH_RESET_EMUL_N_BANKS(_n) * TT_BH_RESET_BANK_STRIDE,                    \
	};                                                                                         \
                                                                                                   \
	BUILD_ASSERT(TT_BH_RESET_EMUL_MASK_DEFINE(_n) != 0, "reset mask should never be zero");    \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(_n, tt_bh_reset_emul_init, NULL, &tt_bh_reset_emul_data_##_n,        \
			      &tt_bh_reset_emul_config_##_n, PRE_KERNEL_1,                         \
			      CONFIG_RESET_INIT_PRIORITY, &tt_bh_reset_emul_api);

DT_INST_FOREACH_STATUS_OKAY(TT_BH_RESET_EMUL_DEFINE);
