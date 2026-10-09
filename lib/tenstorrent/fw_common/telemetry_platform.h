/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TELEMETRY_PLATFORM_H
#define TELEMETRY_PLATFORM_H

#include <stdint.h>

/* Hooks the common telemetry core (telemetry.c) needs from each SoC. */

/** Slot of @p tag in the platform's value storage, or -1 if the platform does not implement it. */
int telemetry_platform_slot(uint16_t tag);

/** Value storage, indexed by the slot returned from telemetry_platform_slot(). */
uint32_t *telemetry_platform_data(void);

/** Fill the values that do not change at runtime and the table header. */
void telemetry_platform_write_static(uint32_t app_version);

/** Refresh the dynamic values. */
void telemetry_platform_update(void);

/** Make the table and data addresses discoverable by the host. */
void telemetry_platform_publish(void);

/** Current update interval in ms (provided by the common core). */
uint32_t telemetry_update_interval_ms(void);

/** Store @p value for @p tag; ignored if the platform does not implement the tag. */
void telemetry_set(uint16_t tag, uint32_t value);

/** Value of @p tag, or 0 if the platform does not implement it. */
uint32_t telemetry_get(uint16_t tag);

#endif
