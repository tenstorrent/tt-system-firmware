/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DVFS_H
#define DVFS_H

#include <stdbool.h>

extern bool dvfs_enabled;

void StartDVFSTimer(void);
void AdjustDVFSTimer(void);
void DVFSChange(void);

#endif
