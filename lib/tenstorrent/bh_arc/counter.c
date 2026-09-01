/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aiclk_ppm.h"
#include "dvfs.h"

#include <tenstorrent/smc_msg.h>
#include <tenstorrent/msgqueue.h>

static uint8_t counter_handler(const union request *request, struct response *response)
{
	switch (request->counter.counter_bank) {
	case COUNTER_BANK_THROTTLERS:
		return throttler_counter_handler(request, response);
	case COUNTER_BANK_DVFS:
		return dvfs_counter_handler(request, response);
	default:
		return 1;
	}
}

REGISTER_MESSAGE(TT_SMC_MSG_COUNTER, counter_handler);
