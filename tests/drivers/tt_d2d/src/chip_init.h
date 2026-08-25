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
 * interconnect, then opens the SMC address filters. Both are prerequisites of
 * tt_d2d_load_fw() and neither belongs to the D2D driver: they are chip-wide,
 * and in a shipping system BL1 will have done them long before any tile is
 * loaded. The test does them itself so it can run standalone.
 */
void tt_d2d_test_chip_init(void);

#endif /* TT_D2D_TEST_CHIP_INIT_H_ */
