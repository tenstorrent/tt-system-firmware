/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TENSTORRENT_GR_SMC_H_
#define TENSTORRENT_GR_SMC_H_

#include <stdint.h>
#include <zephyr/toolchain.h>

FUNC_NORETURN void gr_smc_jump_to(uintptr_t entry_point);

#endif
