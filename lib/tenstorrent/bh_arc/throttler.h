/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef THROTTLER_H
#define THROTTLER_H

#include <stdint.h>
#include <stdbool.h>

void InitThrottlers(void);
void CalculateThrottlers(void);
int32_t Dm2CmSetBoardPowerLimit(const uint8_t *data, uint8_t size);
uint8_t ThrottlerSetKernelThrottlerEnabled(uint32_t enabled);
uint8_t ThrottlerSetKernelThrottlerStopFreq(uint32_t frequency);
uint32_t GetStartNOPCount(void);
uint32_t GetNOPOnAccumulatedTime(void);
/* ms NOP was on during the last telemetry update window, clamped to window_ms */
uint32_t GetNOPOnDuration(uint32_t window_ms);

#endif
