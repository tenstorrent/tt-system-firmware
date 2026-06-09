/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CAT_H
#define CAT_H

#include <tenstorrent/bh_arc.h>

#define T_J_SHUTDOWN 110 /* BH Prod Spec 7.3 */

#define CAT_GDDR_THERM_TRIP_TEMP          GDDR_THERM_TRIP_TEMP
#define CAT_GDDR_THERM_TRIP_CRITICAL_TEMP GDDR_THERM_TRIP_CRITICAL_TEMP
#define CAT_GDDR_THERM_TRIP_DURATION_MS   (GDDR_THERM_TRIP_DURATION_MIN * 60 * 1000)

void StartGddrThermTripMonitor(void);
int MonitorGddrThermTrip(int64_t now, int max_temp);

#endif
