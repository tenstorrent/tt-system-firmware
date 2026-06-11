/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TELEMETRY_INTERNAL_H
#define TELEMETRY_INTERNAL_H

#include <stdint.h>

typedef struct {
	float vcore_voltage;    /* mV */
	float vcore_power;      /* W */
	float vcore_current;    /* A */
	float asic_temperature; /* degC */
	float gddr_io_current;
} TelemetryInternalData;

void ReadTelemetryInternal(int64_t max_staleness, TelemetryInternalData *data);

#endif
