/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CAT_H
#define CAT_H

#include <stdbool.h>

#include <tenstorrent/bh_arc.h>

#define T_J_SHUTDOWN 110 /* BH Prod Spec 7.3 */

void StartGddrThermTripMonitor(void);

/** @brief Evaluate the GDDR thermal-trip state machine for one sample against
 * the supplied thresholds.
 *
 * Production code uses the internal MonitorGddrThermTrip wrapper, which supplies
 * the thresholds seeded from the firmware table at init.
 *
 * @param now            Current uptime in milliseconds.
 * @param max_temp       Highest observed GDDR temperature in degrees Celsius.
 * @param trip_temp      Sustained over-temp threshold in degrees Celsius.
 * @param critical_temp  Instantaneous (no-dwell) trip threshold in degrees Celsius.
 * @param duration_ms    Sustained dwell time before tripping, in milliseconds.
 * @return The tripping temperature (non-zero) when a trip fires, otherwise 0.
 */
int EvaluateGddrThermTrip(int64_t now, int max_temp, int trip_temp, int critical_temp,
			  int64_t duration_ms);

/** @brief Enable or disable the GDDR thermal-trip action at runtime.
 *
 * @param enabled false to disable, true to enable.
 * @return 0 on success.
 */
uint8_t CatSetGddrThermTripEnabled(bool enabled);

#endif
