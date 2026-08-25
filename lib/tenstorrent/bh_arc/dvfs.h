/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DVFS_H
#define DVFS_H

#include <stdbool.h>

extern bool dvfs_enabled;

void InitDVFS(void);
void StartDVFSTimer(void);
void AdjustDVFSTimer(void);
void DVFSChange(void);

struct response;
union request;
uint8_t dvfs_counter_handler(const union request *request, struct response *response);

#endif
