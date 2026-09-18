/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * What the Sival drop's own init does before it goes near a D2D tile, called
 * out of the drop's libraries rather than restated here.
 *
 * The two platforms differ by more than register names. On Mimir the D2D tiles
 * hang off the management network directly, so lifting the cold resets over
 * them and their interconnect and opening the firewall filters is the whole
 * job. On Keraunos each D2D sits inside an HSIO tile with its own reset
 * controller and clock, none of which answers until the tile around it is up,
 * so the same job runs one level deeper and in a stricter order.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#include <basic_init.h>
#include <clock.h>
#include <firewall.h>
#include <platform.h>

#include "chip_init.h"

#if defined(CONFIG_SOC_TT_KERAUNOS_SMC)

#include <hsio_tile_init.h>

/*
 * The tile the K2M link leaves on, and with it the HSIO tile it lives in: the
 * D2D instance and the HSIO tile carry the same index, and the reference
 * bring-up for this package uses 2.
 */
#define KER_LINK_HSIO_TILE 2U

/*
 * Every HSIO tile from 0 up to the target has to be out of cold reset, not
 * just the target: they sit on the management network ring in order, and the
 * ring has to be continuous for SMC traffic to reach the far one at all.
 */
#define KER_HSIO_RESET_MASK                                                                        \
	(KER_RST_HSIO0 | (KER_RST_HSIO0 << 1) | (KER_RST_HSIO0 << KER_LINK_HSIO_TILE))

int tt_d2d_test_chip_init(void)
{
	grendel_err_t err;

	/*
	 * Blocks sample their straps at whatever rate is running as they come
	 * out of cold reset, so the reference clock stays selected until the
	 * tile is assembled.
	 */
	config_cce_clock(0);

	smc_release_reset(KER_RST_SMN | KER_HSIO_RESET_MASK | KER_RST_D2D2);

	/* Refclk has to reach the tile before any register in it is touched. */
	enable_hsio_clk(KER_LINK_HSIO_TILE, 0);

	/*
	 * The SMC filters and the two SMN filters in front of the tile, rather
	 * than disable_ker_smn_filters(), which also walks the PCIe filters --
	 * those live behind PCIe's own cold reset and hang the bus when it is
	 * still held, which here it is.
	 */
	err = disable_ker_smc_filters();
	if (err != GRENDEL_ERR_OK) {
		return -EIO;
	}

	err = disable_firewall_filters(KER_SMN_HSIO2HSIO, KER_LINK_HSIO_TILE);
	if (err != GRENDEL_ERR_OK) {
		return -EIO;
	}

	err = disable_firewall_filters(KER_SMN_HSIO2SMN, KER_LINK_HSIO_TILE);
	if (err != GRENDEL_ERR_OK) {
		return -EIO;
	}

	/*
	 * Inside the tile now. The SMC reset unit only ungates the tile as a
	 * whole; its blocks come out through the tile's own controller.
	 */
	hsio_tile_release_cold_reset(KER_LINK_HSIO_TILE,
				     HSIO_RST_CCE0 | HSIO_RST_CCE1 | HSIO_RST_TL1 |
					     HSIO_RST_FABRIC);

	/*
	 * Unlike Mimir, the switch to the PLL belongs here rather than after
	 * the D2D straps are released: the straps are inside the tile and do
	 * not answer until it is clocked and its filters are open. See
	 * tt_d2d_test_chip_clock_up().
	 */
	config_cce_clock(1);
	enable_hsio_clk(KER_LINK_HSIO_TILE, 1);

	err = disable_ker_hsio_filters(KER_LINK_HSIO_TILE);
	if (err != GRENDEL_ERR_OK) {
		return -EIO;
	}

	return 0;
}

int tt_d2d_test_chip_clock_up(void)
{
	/* Already done above, where this platform needs it. */
	return 0;
}

#else /* Mimir */

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

int tt_d2d_test_chip_clock_up(void)
{
	config_cce_clock(1);

	return 0;
}

#endif
