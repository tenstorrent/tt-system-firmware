/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * What mimir_init() in the Sival drop does before it goes near a D2D tile,
 * called out of the drop's own basic_init library rather than restated here:
 * lift the cold resets covering the tiles and their interconnect, then open
 * the SMC and ITN firewall filters.
 */

#include <errno.h>

#include <basic_init.h>

#include "chip_init.h"

int tt_d2d_test_chip_init(void)
{
	grendel_err_t err;

	smc_release_reset(MIMIR_RST_SMN | MIMIR_RST_ITN | MIMIR_RST_D2D0 | MIMIR_RST_D2D1);

	err = disable_all_mimir_firewall_filters();
	if (err != GRENDEL_ERR_OK) {
		return -EIO;
	}

	return 0;
}
