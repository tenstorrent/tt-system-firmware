/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_bh_efuse

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/misc/bh_efuse.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

LOG_MODULE_REGISTER(tt_bh_efuse, CONFIG_TT_BH_EFUSE_LOG_LEVEL);

#if DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) > 0

#define EFUSE_DFT0_MEM_BASE_ADDR          0x80040000u
#define EFUSE_DFT0_CNTL_REG_MAP_BASE_ADDR 0x80048000u
#define EFUSE_BOX_ADDR_ALIGN              0x2000u

#define EFUSE_CTRL_REG_START_ADDR(box_id)                                                          \
	(EFUSE_DFT0_CNTL_REG_MAP_BASE_ADDR + ((uint32_t)(box_id) * EFUSE_BOX_ADDR_ALIGN))

#define EFUSE_RD_CNTL_REG_OFFSET 0x0u
#define EFUSE_DATA_REG_OFFSET    0xCu

#define GET_EFUSE_CNTL_ADDR(box_id, reg_name)                                                      \
	(EFUSE_##reg_name##_REG_OFFSET + EFUSE_CTRL_REG_START_ADDR(box_id))

typedef struct {
	uint32_t csb: 1;
	uint32_t load: 1;
	uint32_t rsvd_0: 6;
	uint32_t strobe: 1;
	uint32_t rsvd_1: 7;
	uint32_t addr: 13;
	uint32_t rsvd_2: 2;
	uint32_t ovrd: 1;
} EFUSE_CNTL_EFUSE_RD_CNTL_reg_t;

typedef union {
	uint32_t val;
	EFUSE_CNTL_EFUSE_RD_CNTL_reg_t f;
} EFUSE_CNTL_EFUSE_RD_CNTL_reg_u;

#define EFUSE_CNTL_EFUSE_RD_CNTL_REG_DEFAULT 0x00000001u

static int tt_bh_efuse_read_impl(const struct device *dev, enum tt_bh_efuse_access_type acc_type,
				 enum tt_bh_efuse_box_id box_id, uint32_t offset, uint32_t *value)
{
	ARG_UNUSED(dev);

	if ((uint32_t)box_id >= TT_BH_EFUSE_BOX_ID_NUM) {
		return -EINVAL;
	}

	if (acc_type == TT_BH_EFUSE_ACCESS_DIRECT) {
		uint32_t efuse_addr = EFUSE_DFT0_MEM_BASE_ADDR +
				      ((uint32_t)box_id * EFUSE_BOX_ADDR_ALIGN) +
				      (offset * sizeof(uint32_t));

		*value = sys_read32(efuse_addr);
		return 0;
	}

	if (acc_type != TT_BH_EFUSE_ACCESS_INDIRECT) {
		return -EINVAL;
	}

	EFUSE_CNTL_EFUSE_RD_CNTL_reg_u efuse_rd_cntl_reg;

	efuse_rd_cntl_reg.val = EFUSE_CNTL_EFUSE_RD_CNTL_REG_DEFAULT;
	efuse_rd_cntl_reg.f.csb = 0;
	efuse_rd_cntl_reg.f.load = 1;
	efuse_rd_cntl_reg.f.addr = offset;
	efuse_rd_cntl_reg.f.ovrd = 1;

	sys_write32(efuse_rd_cntl_reg.val, GET_EFUSE_CNTL_ADDR(box_id, RD_CNTL));
	k_busy_wait(1);

	efuse_rd_cntl_reg.f.strobe = 1;
	sys_write32(efuse_rd_cntl_reg.val, GET_EFUSE_CNTL_ADDR(box_id, RD_CNTL));
	k_busy_wait(1);
	efuse_rd_cntl_reg.f.strobe = 0;
	sys_write32(efuse_rd_cntl_reg.val, GET_EFUSE_CNTL_ADDR(box_id, RD_CNTL));
	k_busy_wait(1);

	efuse_rd_cntl_reg.f.ovrd = 0;
	sys_write32(efuse_rd_cntl_reg.val, GET_EFUSE_CNTL_ADDR(box_id, RD_CNTL));

	*value = sys_read32(GET_EFUSE_CNTL_ADDR(box_id, DATA));
	return 0;
}

static int tt_bh_efuse_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static DEVICE_API(tt_efuse, tt_bh_efuse_api) = {
	.read = tt_bh_efuse_read_impl,
};

#define TT_BH_EFUSE_DEFINE(inst)                                                                   \
	DEVICE_DT_INST_DEFINE(inst, tt_bh_efuse_init, NULL, NULL, NULL, POST_KERNEL,               \
			      CONFIG_TT_BH_EFUSE_INIT_PRIORITY, &tt_bh_efuse_api);

DT_INST_FOREACH_STATUS_OKAY(TT_BH_EFUSE_DEFINE)

#endif /* DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) > 0 */
