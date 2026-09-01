/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <tenstorrent/msgqueue.h>
#include "vf_curve.h"
#include "throttler.h"
#include "aiclk_ppm.h"
#include "voltage.h"

bool dvfs_enabled;

/* DVFS timer ticks that expired while the previous pass was still queued. Incremented from the
 * timer ISR and read/cleared from the message-handler thread, so it must be atomic: a plain
 * read-modify-write could drop a clear that lands between the ISR's load and store.
 */
static atomic_t dvfs_dropped_ticks;

/* Worst-case DVFS pass spacing and duration since the last clear, in microseconds. Updated only
 * from the work handler, but cleared from whichever thread services the counter message, so the
 * maxima are published atomically.
 */
static atomic_t dvfs_max_period_us;
static atomic_t dvfs_max_pass_us;

/* Raise *target to value if value is larger. Retries on contention with a concurrent clear. */
static void atomic_max_u32(atomic_t *target, uint32_t value)
{
	atomic_val_t old = atomic_get(target);

	while (value > (uint32_t)old) {
		if (atomic_cas(target, old, (atomic_val_t)value)) {
			break;
		}
		old = atomic_get(target);
	}
}

void DVFSChange(void)
{
	CalculateThrottlers();
	CalculateTargAiclk();

	uint32_t targ_freq = GetAiclkTarg();
	uint32_t aiclk_voltage = VFCurve(targ_freq);

	VoltageArbRequest(VoltageReqAiclk, aiclk_voltage);

	CalculateTargVoltage();

	DecreaseAiclk();
	VoltageChange();
	IncreaseAiclk();
}

static void dvfs_work_handler(struct k_work *work)
{
	static uint32_t prev_start_cyc;
	static bool have_prev;
	uint32_t start_cyc = k_cycle_get_32();

	/* Unsigned subtraction stays correct across the 32-bit cycle counter wrap, since one
	 * period or pass is tiny relative to the counter's range.
	 */
	if (have_prev) {
		atomic_max_u32(&dvfs_max_period_us,
			       k_cyc_to_us_floor32(start_cyc - prev_start_cyc));
	}
	prev_start_cyc = start_cyc;
	have_prev = true;

	DVFSChange();

	atomic_max_u32(&dvfs_max_pass_us, k_cyc_to_us_floor32(k_cycle_get_32() - start_cyc));
}
static K_WORK_DEFINE(dvfs_worker, dvfs_work_handler);

static void dvfs_timer_handler(struct k_timer *timer)
{
	/* 0 means the work item was already queued, so this tick is folded into the pending one
	 * and the DVFS loop skips a cycle.
	 */
	if (k_work_submit(&dvfs_worker) == 0) {
		atomic_inc(&dvfs_dropped_ticks);
	}
}
static K_TIMER_DEFINE(dvfs_timer, dvfs_timer_handler, NULL);

void InitDVFS(void)
{
	InitVFCurve();
	InitVoltagePPM();
	InitArbMaxVoltage();
	InitThrottlers();
	dvfs_enabled = true;
}

#define DVFS_MSEC 1

void StartDVFSTimer(void)
{
	k_timer_start(&dvfs_timer, K_MSEC(DVFS_MSEC), K_MSEC(DVFS_MSEC));
}

#define DVFS_TICKS (CONFIG_SYS_CLOCK_TICKS_PER_SEC * DVFS_MSEC / MSEC_PER_SEC)

/* If DVFS is already scheduled "close enough" to the board power message, then don't try to adjust
 * it. There may be some jitter in the message arrival and we don't want to suddenly go from being
 * very close to very far away. 10% is arbitrary.
 */
#define DVFS_ADJUSTMENT_THRESHOLD (DVFS_TICKS * 10 / 100) /* 10% of DVFS interval */

/* DVFS's PID controllers assume they are run on a 1ms interval. Changing the interval implicitly
 * changes their behaviour. 1% should be small enough to not cause trouble.
 */
#define DVFS_ADJUSTMENT_STEP (DVFS_TICKS * 1 / 100) /* 1% of DVFS interval */

void AdjustDVFSTimer(void)
{
	/* We just received a board power update from the DMC. If DVFS is still more than 10% of
	 * its interval away, then reduce that time by 1%. Over enough cycles, this should bring
	 * the DMC->DVFS latency down.
	 */
	if (dvfs_enabled) {
		k_ticks_t dvfs_remaining = k_timer_remaining_ticks(&dvfs_timer);

		if (dvfs_remaining > DVFS_ADJUSTMENT_THRESHOLD) {
			k_timeout_t delay = K_TICKS(dvfs_remaining - DVFS_ADJUSTMENT_STEP);

			k_timer_start(&dvfs_timer, delay, K_MSEC(DVFS_MSEC));
		}
	}
}

/* Indexed by @ref dvfs_counter_index so GET and CLEAR need no per-counter switch. */
static atomic_t *const dvfs_counters[DVFS_COUNTER_COUNT] = {
	[DVFS_COUNTER_DROPPED_TICKS] = &dvfs_dropped_ticks,
	[DVFS_COUNTER_MAX_PERIOD_US] = &dvfs_max_period_us,
	[DVFS_COUNTER_MAX_PASS_US] = &dvfs_max_pass_us,
};

uint8_t dvfs_counter_handler(const union request *request, struct response *response)
{
	switch (request->counter.command) {
	case COUNTER_CMD_GET:
		if (request->counter.bank_index >= DVFS_COUNTER_COUNT) {
			return 1;
		}
		/* This bank tracks neither overflow nor freeze, so both status fields read 0. */
		response->data[1] = 0;
		response->data[2] =
			(uint32_t)atomic_get(dvfs_counters[request->counter.bank_index]);
		break;
	case COUNTER_CMD_CLEAR:
		for (uint32_t i = 0; i < DVFS_COUNTER_COUNT; i++) {
			if (request->counter.mask & BIT(i)) {
				atomic_clear(dvfs_counters[i]);
			}
		}
		break;
	case COUNTER_CMD_FREEZE:
	default:
		/* FREEZE is rejected rather than ignored: these counters must keep running for a
		 * measurement window to mean anything
		 */
		return 1;
	}

	return 0;
}
