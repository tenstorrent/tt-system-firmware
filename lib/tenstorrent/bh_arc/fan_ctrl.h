/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CTRL_H
#define FAN_CTRL_H

#include <stdint.h>

int init_fan_ctrl(void);
uint32_t GetFanSpeed(void);
uint16_t GetFanRPM(void);
void SetFanRPM(uint16_t rpm);
void DmcFanSpeedFeedback(uint32_t speed_percentage);

#endif
