/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TESTS_DRIVERS_TT_D2D_D2D_FW_BLOB_H_
#define TESTS_DRIVERS_TT_D2D_D2D_FW_BLOB_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Get the D2D firmware image embedded in this test.
 *
 * @param[out] size Length of the image in bytes, or NULL if not wanted
 *
 * @return Pointer to the image, suitable for passing to tt_d2d_load_fw()
 */
const uint8_t *d2d_fw_image(size_t *size);

#endif /* TESTS_DRIVERS_TT_D2D_D2D_FW_BLOB_H_ */
