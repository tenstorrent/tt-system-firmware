/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TT_D2D_TEST_CHIP_INIT_H_
#define TT_D2D_TEST_CHIP_INIT_H_

/**
 * @brief Make the D2D tiles reachable from this SMC.
 *
 * Lifts the subsystem cold resets covering the D2D tiles and their
 * interconnect, then opens the firewall filters in front of them. Both are
 * prerequisites of tt_d2d_load_fw() and neither belongs to the D2D driver:
 * they are chip-wide, and in a shipping system BL1 will have done them long
 * before any tile is loaded. The test does them itself so it can run
 * standalone.
 *
 * On Keraunos this also covers the HSIO tile the D2D sits behind, which Mimir
 * has no equivalent of.
 *
 * @return 0 on success, -EIO if the drop rejected a filter configuration.
 */
int tt_d2d_test_chip_init(void);

/**
 * @brief Switch the CCE clock from the reference to the PLL.
 *
 * Called after the D2D straps are released. The firmware brings up its own
 * PLL against this clock and reports a lock failure if it is still running off
 * the reference, so it has to happen before an image is loaded.
 *
 * It is a separate call from tt_d2d_test_chip_init() because the two platforms
 * want it on opposite sides of the strap release. Mimir samples the straps at
 * whatever rate is running when they come out, so it releases them on the
 * reference and switches afterwards -- which is what this does. Keraunos puts
 * its D2D behind an HSIO tile that has to be clocked and have its firewalls
 * open before the straps answer at all, so there the switch has already
 * happened inside tt_d2d_test_chip_init() and this is a no-op. Callers get one
 * order that is correct on both.
 *
 * @return 0 on success.
 */
int tt_d2d_test_chip_clock_up(void);

#endif /* TT_D2D_TEST_CHIP_INIT_H_ */
