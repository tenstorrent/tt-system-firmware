/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_TT_SMC_REMOTEPROC_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_TT_SMC_REMOTEPROC_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

/**
 * @file
 * @brief Tenstorrent SMC Remote Processor APIs
 */

/**
 * @brief Write binary data into remote SMC memory.
 *
 * Copy an arbitrary block of binary data into the remote SMC address space at @p addr.
 *
 * @param dev Pointer to the tt_smc_remoteproc device.
 * @param addr Target address in the remote SMC memory map.
 * @param bin_data Pointer to the binary data to write.
 * @param bin_size Number of bytes in @p bin_data.
 *
 * @return 0 on success, or a negative errno code on failure.
 */
int tt_smc_remoteproc_load(const struct device *dev, uint64_t addr, const uint8_t *bin_data,
			   size_t bin_size);

/**
 * @brief Boot a previously loaded SMC image.
 *
 * Start execution of the image already placed at @p addr on the remote SMC CPU.
 *
 * @param dev Pointer to the tt_smc_remoteproc device.
 * @param addr Address of the loaded image entry point in the remote SMC memory map.
 *
 * @return 0 on success, or a negative errno code on failure.
 */
int tt_smc_remoteproc_boot(const struct device *dev, uint64_t addr);

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_TT_SMC_REMOTEPROC_H_ */
