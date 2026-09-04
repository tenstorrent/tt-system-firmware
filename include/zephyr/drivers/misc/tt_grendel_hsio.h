/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_TT_GRENDEL_HSIO_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_TT_GRENDEL_HSIO_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring one HSIO tile out of reset and open its host-path firewalls.
 *
 * @param hsio_tile Tile index (0-4).
 *
 * @retval 0 Tile programmed.
 * @retval -EINVAL Invalid tile.
 * @retval -EIO SiVal programming failed.
 */
int tt_grendel_hsio_init(uint32_t hsio_tile);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_TT_GRENDEL_HSIO_H_ */
