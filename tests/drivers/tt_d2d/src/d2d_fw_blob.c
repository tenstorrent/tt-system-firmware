/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Embeds an image in the test so it has something to hand tt_d2d_load_fw().
 * This lives here rather than beside the driver on purpose: a production image
 * gets the firmware as its own bundle entry and passes the driver a pointer to
 * it, so nothing under drivers/ should be able to embed it.
 *
 * d2d_fw.inc holds the image named by this test's CMakeLists, as a list of
 * byte initialisers.
 */

#include <zephyr/toolchain.h>

#include "d2d_fw_blob.h"

/* tt_d2d_load_fw() reads the image with 32-bit loads. */
static const uint8_t d2d_fw_bin[] __aligned(sizeof(uint32_t)) = {
#include "d2d_fw.inc"
};

const uint8_t *d2d_fw_image(size_t *size)
{
	if (size != NULL) {
		*size = sizeof(d2d_fw_bin);
	}

	return d2d_fw_bin;
}
