/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TELEMETRY_INTERNAL_H
#define TELEMETRY_INTERNAL_H

#include <stdint.h>

#include "gddr.h"
#include "regulator.h"

typedef struct {
	float vcore_voltage;          /* mV */
	float vcore_power;            /* W */
	float vcore_current;          /* A */
	float asic_temperature;       /* degC */
	struct gddr_temps gddr_temps; /* per-instance GDDR die temps + max across all dies, degC */
	float rail_power[POWER_RAIL_COUNT]; /* W, indexed by enum power_rail */
} TelemetryInternalData;

void ReadTelemetryInternal(int64_t max_staleness, TelemetryInternalData *data);

#endif
