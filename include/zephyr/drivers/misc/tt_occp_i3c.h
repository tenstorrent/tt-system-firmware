/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TT_ZEPHYR_PLATFORMS_INCLUDE_TENSTORRENT_DRIVERS_TT_OCCP_I3C_H_
#define TT_ZEPHYR_PLATFORMS_INCLUDE_TENSTORRENT_DRIVERS_TT_OCCP_I3C_H_

#include <stdbool.h>

#include <zephyr/device.h>

#include <tenstorrent/occp.h>

#ifdef __cplusplus
extern "C" {
#endif

const struct device *tt_occp_i3c_get_i3c_device(const struct device *dev);

bool tt_occp_i3c_is_initialized(const struct device *dev);

const struct occp_backend *tt_occp_i3c_get_backend(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* TT_ZEPHYR_PLATFORMS_INCLUDE_TENSTORRENT_DRIVERS_TT_OCCP_I3C_H_ */
