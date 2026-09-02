/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MEMC_MEMC_TT_MIMIR_H_
#define ZEPHYR_INCLUDE_DRIVERS_MEMC_MEMC_TT_MIMIR_H_

#include <zephyr/device.h>

#include "err.h"
#include "fw_params.h"
#include "gddr_common_types.h"
#include "gddr_backend.h"
#include "gddr_types.h"

/**
 * @brief Install the platform GDDR configuration for the controller.
 *
 * Stores the backend and fw_params in the device's data; the device passes them
 * to gddr_init() at its own init. Call once, before the memc device initializes.
 *
 * The backend and fw_params are runtime/platform-supplied (host-written params,
 * environment-selected backend), which is why they arrive through this API
 * rather than devicetree. @p params is non-const because this call fills in
 * gddr_tile_mask from the controller's own DT topology (the mem-tiles).
 *
 * @param dev     The memc controller device.
 * @param backend Caller-owned backend (non-NULL, with a non-NULL get_blob).
 * @param params  fw_params minus gddr_tile_mask
 * @return 0 on success, -EINVAL if @p backend or @p params is NULL.
 */
int memc_tt_mimir_set_config(const struct device *dev, const gddr_backend_t *backend,
			     fw_params_t *params);

#endif /* ZEPHYR_INCLUDE_DRIVERS_MEMC_MEMC_TT_MIMIR_H_ */
