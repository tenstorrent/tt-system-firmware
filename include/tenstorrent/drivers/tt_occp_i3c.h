/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_MISC_TT_OCCP_I3C_H_
#define ZEPHYR_DRIVERS_MISC_TT_OCCP_I3C_H_

#include <zephyr/device.h>
#include <zephyr/drivers/i3c.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TT OCCP I3C Device Driver API
 * @defgroup tt_occp_i3c_interface TT OCCP I3C Device Driver API
 * @ingroup misc_interfaces
 * @{
 */

/**
 * @brief Get the I3C device associated with this TT OCCP device
 *
 * @param dev TT OCCP I3C device
 * @return Pointer to I3C device, or NULL if not available
 */
const struct device *tt_occp_i3c_get_i3c_device(const struct device *dev);

/**
 * @brief Check if the TT OCCP I3C device is initialized
 *
 * @param dev TT OCCP I3C device
 * @return true if initialized, false otherwise
 */
bool tt_occp_i3c_is_initialized(const struct device *dev);

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_MISC_TT_OCCP_I3C_H_ */
