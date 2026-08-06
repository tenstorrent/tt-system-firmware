/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LIB_TENSTORRENT_BH_ARC_INIT_H_
#define LIB_TENSTORRENT_BH_ARC_INIT_H_

#include "status_reg.h"

#include <stdint.h>

#define SCRATCHPAD_SIZE CONFIG_TT_BH_ARC_SCRATCHPAD_SIZE

typedef enum {
	kHwInitNotStarted = 0,
	kHwInitStarted = 1,
	kHwInitDone = 2,
	kHwInitError = 3,
} HWInitStatus;

typedef enum {
	FW_ID_SMC_NORMAL = 0,
	FW_ID_SMC_RECOVERY = 1,
} FWID;

/*
 * Identifiers for the SMC init stages. Each enum value is the bit position
 * in STATUS_ERROR_STATUS0 for that stage's failure.
 */
enum init_stage_id {
	INIT_STAGE_REGULATOR = 0,
	INIT_STAGE_CABLE_FAULT = 1,
	INIT_STAGE_TENSIX = 2,
	INIT_STAGE_MRISC_LOAD = 3,
	INIT_STAGE_GDDR_TRAIN = 4,
	INIT_STAGE_COUNT,
};

extern uint32_t error_status0;

/**
 * @brief Record a failure from an init function
 *
 * Sets bit @p stage in STATUS_ERROR_STATUS0 and writes the shadow value
 * through to scratch RAM immediately so the host can read it even if init
 * hangs after this call. Safe to call multiple times; the bitmap accumulates.
 */
void record_init_failure(enum init_stage_id stage);

#endif
