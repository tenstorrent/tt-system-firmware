/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_TT_D2D_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_TT_D2D_H_

/**
 * @file
 * @brief Tenstorrent Grendel die-to-die (D2D) firmware loading APIs
 *
 * Bringing a D2D link up takes three steps, kept separate because each has to
 * be sequenced against work this driver does not own:
 *
 *   1. tt_d2d_reset_release() - deassert the tile's subsystem resets. Must
 *      happen before the CCE clock switches to the PLL, because the D2D logic
 *      samples its straps at the clock rate in effect at the time.
 *   2. tt_d2d_load_fw()       - put firmware in the Rocket's SRAM and fill in
 *      its configuration block. Leaves the Rocket in reset.
 *   3. tt_d2d_start()         - release the Rocket so the firmware runs.
 *
 * Step 3 is deliberately not folded into step 2. Both ends of a link must be
 * started close together, and with sideband synchronisation disabled (as it is
 * under emulation) there is nothing in hardware to make that happen: starting
 * each Rocket as soon as its own image landed has been observed to leave
 * Keraunos and the far Mimir unable to train. The caller is expected to load
 * every tile first and start them as a group.
 *
 * Two things have to be true before any of this works, and neither is done
 * here because neither belongs to a single tile:
 *
 *   - the SMC cold resets covering the tile and its interconnect must be
 *     lifted, and
 *   - the SMC inbound/outbound filters must permit the tile's address range.
 *
 * Until both hold, the tile does not answer at all and tt_d2d_load_fw() will
 * fail its reachability probe with -ENODEV rather than appearing to succeed.
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

/**
 * @brief Deassert a D2D tile's subsystem resets.
 *
 * Brings the tile's NOC, APB, system, link-layer, AXI and QNP resets out one
 * at a time, in the order the hardware requires. Until this runs the tile does
 * not reliably answer accesses, so it must precede tt_d2d_load_fw().
 *
 * @param dev D2D tile device
 *
 * @retval 0 on success
 */
int tt_d2d_reset_release(const struct device *dev);

/**
 * @brief Load firmware into a D2D tile, leaving it halted.
 *
 * Holds the tile's Rocket in reset, clears its SRAM, copies @p img in,
 * optionally reads it back to confirm it landed, and writes the configuration
 * block the firmware reads at startup. Call tt_d2d_start() to run it.
 *
 * @param dev D2D tile device
 * @param img Firmware image
 * @param img_size Size of @p img in bytes. Must be a multiple of 4 and must
 *                 leave the configuration block at the top of SRAM untouched.
 *
 * @retval 0 on success
 * @retval -EINVAL if @p img is NULL, empty, or not a multiple of 4 bytes
 * @retval -ENOSPC if @p img would overrun the configuration block
 * @retval -ENODEV if the tile does not answer a write to its SRAM
 * @retval -EIO if the image read back from SRAM does not match @p img
 */
int tt_d2d_load_fw(const struct device *dev, const uint8_t *img, size_t img_size);

/**
 * @brief Release a D2D tile's Rocket so loaded firmware begins executing.
 *
 * @param dev D2D tile device
 *
 * @retval 0 on success
 */
int tt_d2d_start(const struct device *dev);

#if defined(CONFIG_TT_D2D_FW_BLOB) || defined(__DOXYGEN__)

/*
 * Emitted by d2d_fw_blob.S from the image named by CONFIG_TT_D2D_FW_BLOB_PATH.
 * The names follow objcopy's convention for an incorporated binary because the
 * Sival library imports them under exactly these names instead of holding its
 * own copy of the firmware.
 */
extern const uint8_t _binary_d2d_fw_bin_start[];
extern const uint8_t _binary_d2d_fw_bin_end[];

/**
 * @brief Get the D2D firmware image linked into this build.
 *
 * Callers within this repository should prefer this to naming the linker
 * symbols, which exist in the form they do for the benefit of an external
 * library.
 *
 * @param[out] size Length of the image in bytes
 *
 * @return Pointer to the image, suitable for passing to tt_d2d_load_fw()
 */
static inline const uint8_t *tt_d2d_fw_image(size_t *size)
{
	*size = (size_t)(_binary_d2d_fw_bin_end - _binary_d2d_fw_bin_start);

	return _binary_d2d_fw_bin_start;
}

#endif /* CONFIG_TT_D2D_FW_BLOB */

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_TT_D2D_H_ */
