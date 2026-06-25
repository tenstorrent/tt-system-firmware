/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_BH_EFUSE_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_BH_EFUSE_H_

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

enum tt_bh_efuse_box_id {
	TT_BH_EFUSE_BOX_DFT0 = 0,
	TT_BH_EFUSE_BOX_DFT1 = 1,
	TT_BH_EFUSE_BOX_FUNC = 2,
	TT_BH_EFUSE_BOX_ID_NUM = 3,
};

enum tt_bh_efuse_access_type {
	TT_BH_EFUSE_ACCESS_INDIRECT = 0,
	TT_BH_EFUSE_ACCESS_DIRECT = 1,
};

__subsystem struct tt_efuse_driver_api {
	int (*read)(const struct device *dev, enum tt_bh_efuse_access_type acc_type,
		    enum tt_bh_efuse_box_id box_id, uint32_t offset, uint32_t *value);
};

static inline int tt_bh_efuse_read(const struct device *dev, enum tt_bh_efuse_access_type acc_type,
				   enum tt_bh_efuse_box_id box_id, uint32_t offset, uint32_t *value)
{
	if (value == NULL) {
		return -EINVAL;
	}

	if (dev == NULL || !device_is_ready(dev)) {
		return -ENODEV;
	}

	const struct tt_efuse_driver_api *api = DEVICE_API_GET(tt_efuse, dev);

	if (api == NULL || api->read == NULL) {
		return -ENOSYS;
	}

	return api->read(dev, acc_type, box_id, offset, value);
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_BH_EFUSE_H_ */
