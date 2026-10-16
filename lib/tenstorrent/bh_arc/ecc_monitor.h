/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ECC_MONITOR_H
#define ECC_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "noc_ecc.h"

/**
 * @brief Tensix ECC error totals since boot, accumulated in software.
 *
 * Hardware counters are read and zeroed every time a tile's interrupt is serviced, so they
 * only ever hold the delta since the last visit and these are the record. Saturating at
 * UINT32_MAX.
 */
struct ecc_totals {
	/** NOC 0 NIU header/parity errors over every Tensix NIU, indexed by NOC_ECC_MEM_PARITY etc.
	 */
	uint32_t noc[NOC_ECC_NUM_SOURCES];
	/** Tensix L1 single-bit (corrected) errors over every tile. */
	uint32_t l1_sbe;
	/** Tensix L1 double-bit (uncorrectable) errors over every tile. */
	uint32_t l1_dbe;
};

/**
 * @brief Snapshot the running totals for telemetry.
 *
 * Totals are written only from the system work queue; callers on that queue see a
 * consistent snapshot. Other threads see each word atomically but may straddle an update.
 */
void EccMonitorGetTotals(struct ecc_totals *out);

/**
 * @brief Tell the monitor Tensix tiles were just reinitialised or ungated.
 *
 * Schedules a service pass that re-checks any interrupt group left masked because a
 * tile was clock-gated or the routers were down, and re-arms tiles the pass visits.
 * Safe from any thread; cheap no-op when the monitor is not running.
 */
void EccMonitorTensixChanged(void);

/** Reset unit capacity: eight TENSIX_ERR_INTR_STATUS words, so at most eight vectors. */
#define ECC_MONITOR_TENSIX_IRQ_GROUPS_MAX 8

/**
 * @brief Debug snapshot of the Tensix error interrupt plumbing, for the shell.
 */
struct ecc_monitor_irq_state {
	/** false when the ecc capability is off and nothing below is live. */
	bool running;
	/** Vectors that fired and are waiting for the work item (bit = group). */
	uint32_t pending;
	/** Vectors disabled for the post-service hold-off. */
	uint32_t masked;
	/** Vectors disabled because an asserted tile could not be cleared (gated/routers down). */
	uint32_t blocked;
	/** Vectors wired up in devicetree; valid entries in status[]. */
	uint8_t num_groups;
	/** Live per-tile error level, straight from TENSIX_ERR_INTR_STATUS_n. */
	uint32_t status[ECC_MONITOR_TENSIX_IRQ_GROUPS_MAX];
};

void EccMonitorGetIrqState(struct ecc_monitor_irq_state *out);

#endif /* ECC_MONITOR_H */
