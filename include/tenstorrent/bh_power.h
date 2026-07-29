/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BH_POWER_H
#define BH_POWER_H

#include <stdbool.h>
#include <stdint.h>

enum bh_power_domain {
	BH_POWER_DOMAIN_AICLK,
	BH_POWER_DOMAIN_MRISC,
	BH_POWER_DOMAIN_TENSIX,
	BH_POWER_DOMAIN_L2CPU,
	BH_POWER_DOMAIN_COUNT,
};

int32_t bh_set_l2cpu_enable(bool enable);

/**
 * @brief Returns the power state of the specified domain
 *
 * @param[out] state True if power state is high (busy for AICLK),
 *                   false if power state is low (idle for AICLK)
 *
 * @retval 0 On success
 * @retval -EINVAL If domain is invalid
 */
int bh_power_state_get(enum bh_power_domain domain, bool *state);

#endif
