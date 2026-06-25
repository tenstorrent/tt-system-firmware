/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_bh_efuse_emul

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/misc/bh_efuse.h>

struct tt_bh_efuse_emul_config {
	const uint32_t *dft0; /* flat array of (offset, value) pairs */
	size_t dft0_count;    /* number of pairs */
	const uint32_t *dft1;
	size_t dft1_count;
	const uint32_t *func;
	size_t func_count;
};

static int tt_bh_efuse_emul_read_impl(const struct device *dev,
				      enum tt_bh_efuse_access_type acc_type,
				      enum tt_bh_efuse_box_id box_id, uint32_t offset,
				      uint32_t *value)
{
	const struct tt_bh_efuse_emul_config *cfg = dev->config;
	const uint32_t *src = NULL;

	if (acc_type != TT_BH_EFUSE_ACCESS_DIRECT && acc_type != TT_BH_EFUSE_ACCESS_INDIRECT) {
		return -EINVAL;
	}

	size_t src_count = 0;

	switch (box_id) {
	case TT_BH_EFUSE_BOX_DFT0:
		src = cfg->dft0;
		src_count = cfg->dft0_count;
		break;
	case TT_BH_EFUSE_BOX_DFT1:
		src = cfg->dft1;
		src_count = cfg->dft1_count;
		break;
	case TT_BH_EFUSE_BOX_FUNC:
		src = cfg->func;
		src_count = cfg->func_count;
		break;
	default:
		return -EINVAL;
	}

	for (size_t i = 0; i < src_count; i++) {
		if (src[i * 2] == offset) {
			*value = src[i * 2 + 1];
			return 0;
		}
	}

	*value = 0;
	return 0;
}

static int tt_bh_efuse_emul_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static DEVICE_API(tt_efuse, tt_bh_efuse_emul_api) = {
	.read = tt_bh_efuse_emul_read_impl,
};

#define TT_BH_EFUSE_EMUL_DEFINE(inst)                                                              \
	static const uint32_t tt_bh_efuse_emul_dft0_##inst[] =                                     \
		DT_INST_PROP_OR(inst, dft0_dwords, {});                                            \
	static const uint32_t tt_bh_efuse_emul_dft1_##inst[] =                                     \
		DT_INST_PROP_OR(inst, dft1_dwords, {});                                            \
	static const uint32_t tt_bh_efuse_emul_func_##inst[] =                                     \
		DT_INST_PROP_OR(inst, func_dwords, {});                                            \
	static const struct tt_bh_efuse_emul_config tt_bh_efuse_emul_cfg_##inst = {                \
		.dft0 = tt_bh_efuse_emul_dft0_##inst,                                              \
		.dft0_count = DT_INST_PROP_LEN_OR(inst, dft0_dwords, 0) / 2,                       \
		.dft1 = tt_bh_efuse_emul_dft1_##inst,                                              \
		.dft1_count = DT_INST_PROP_LEN_OR(inst, dft1_dwords, 0) / 2,                       \
		.func = tt_bh_efuse_emul_func_##inst,                                              \
		.func_count = DT_INST_PROP_LEN_OR(inst, func_dwords, 0) / 2,                       \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, tt_bh_efuse_emul_init, NULL, NULL,                             \
			      &tt_bh_efuse_emul_cfg_##inst, POST_KERNEL,                           \
			      CONFIG_TT_BH_EFUSE_INIT_PRIORITY, &tt_bh_efuse_emul_api);

DT_INST_FOREACH_STATUS_OKAY(TT_BH_EFUSE_EMUL_DEFINE)
