/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file board_variables.h
 * @brief Board variable numbering
 */

#ifndef TT_BOARD_VARIABLES_H_
#define TT_BOARD_VARIABLES_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup board_variables Board variables
 * @brief Hardware attributes a flashing tool must verify before writing SPI
 *
 * A board variable is a numbered hardware attribute that, together with the
 * board type, determines whether a firmware image is compatible with the
 * board it is about to be written to. Numbering is common to all board types
 * and ASICs and starts at 0; a number is never reused.
 *
 * SPI writers declare the variables they verified by setting bits in
 * flash_unlock_rqst.verified_variables. Firmware refuses to unlock the flash
 * if the host did not verify every variable that matters on this board.
 *
 * @{
 */

/** @brief JEDEC ID of the SPI flash, as reported by @ref TAG_FLASH_JEDEC_ID.
 *
 * Old images only support Micron MT25QU512ABB. Writing that to a second-
 * source board would cause it to fail to boot, even to recovery.
 */
#define BOARD_VAR_SPI_JEDEC_ID 0

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* TT_BOARD_VARIABLES_H_ */
