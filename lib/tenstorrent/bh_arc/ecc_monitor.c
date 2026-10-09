/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Tensix ECC error monitor.
 *
 * Entirely interrupt driven. Each Tensix tile raises its error line for any armed L1 or
 * NOC NIU ECC event. Nothing touches the NOC from interrupt context.
 *
 * A tile that is clock-gated or in reset cannot interrupt: its ECC_CTRL enables read as
 * zero, and a NOC error landing then leaves the NIU level stuck high with its edge already
 * spent. So every Tensix change (power-on, tile reset, monitor start) re-arms every tile and
 * then sweeps every tile once, collecting what arrived and dropping the levels; that sweep is
 * the only time the monitor reads a tile it was not interrupted for.
 *
 * All counter state lives in software totals written only from the system work queue.
 */

#include "ecc_monitor.h"

#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/zbus/zbus.h>

#include <tenstorrent/sys_init_defines.h>

#include "harvesting.h"
#include "noc.h"
#include "noc_ecc.h"
#include "noc_init.h"
#include "reg.h"
#include "telemetry.h"
#include "tensix_ecc.h"
#include "tensix_state_msg.h"

LOG_MODULE_REGISTER(ecc_monitor);

/*
 * Vectors, priority and the TENSIX_ERR_INTR_STATUS block all come from the devicetree
 * tensix_err_irqs node.
 */
#define TENSIX_ERR_NODE      DT_NODELABEL(tensix_err_irqs)
#define TENSIX_ERR_IRQ_WIRED DT_NODE_HAS_STATUS_OKAY(TENSIX_ERR_NODE)

#if TENSIX_ERR_IRQ_WIRED
#define TENSIX_ERR_IRQ_GROUPS         DT_NUM_IRQS(TENSIX_ERR_NODE)
#define TENSIX_ERR_IRQN(group)        DT_IRQN_BY_IDX(TENSIX_ERR_NODE, group)
#define TENSIX_ERR_IRQ_PRIO(group)    DT_IRQ_BY_IDX(TENSIX_ERR_NODE, group, priority)
#define TENSIX_ERR_STATUS_ADDR(group) (DT_REG_ADDR(TENSIX_ERR_NODE) + sizeof(uint32_t) * (group))

BUILD_ASSERT(TENSIX_ERR_IRQ_GROUPS == DIV_ROUND_UP(NUM_TENSIX_X * NUM_TENSIX_Y, 32),
	     "one interrupt group per 32 Tensix tiles");
BUILD_ASSERT(TENSIX_ERR_IRQ_GROUPS * sizeof(uint32_t) <= DT_REG_SIZE(TENSIX_ERR_NODE),
	     "one TENSIX_ERR_INTR_STATUS word per interrupt group");
BUILD_ASSERT(TENSIX_ERR_IRQ_GROUPS <= ECC_MONITOR_TENSIX_IRQ_GROUPS_MAX,
	     "more vectors than the reset unit has status words");

/* Runtime copy of the vector numbers for irq_enable/irq_disable by group index. */
#define TENSIX_ERR_IRQN_ENTRY(group, _) TENSIX_ERR_IRQN(group)
static const unsigned int tensix_err_irqn[TENSIX_ERR_IRQ_GROUPS] = {
	LISTIFY(TENSIX_ERR_IRQ_GROUPS, TENSIX_ERR_IRQN_ENTRY, (,))};

static void TensixErrIrqEnable(uint32_t group)
{
	irq_enable(tensix_err_irqn[group]);
}

static void TensixErrIrqDisable(uint32_t group)
{
	irq_disable(tensix_err_irqn[group]);
}
#else
#define TENSIX_ERR_IRQ_GROUPS         0
#define TENSIX_ERR_STATUS_ADDR(group) 0

static void TensixErrIrqEnable(uint32_t group)
{
	ARG_UNUSED(group);
}
#endif

/*
 * A vector stays disabled this long after service. With every ECC source armed, a tile with
 * a weak L1 cell or a marginal link would otherwise re-interrupt as fast as the work queue
 * can clear it; this caps a 32-tile group at ten service passes a second.
 */
#define IRQ_REARM_HOLDOFF_MS 100

/* Hardware NIU ECC counters are 16-bit and wrap with no saturate. */
#define NOC_ECC_HW_CNT_MASK GENMASK(15, 0)

static bool monitor_running;

/* Written only on the system work queue. */
static struct ecc_totals totals;

/* ISR -> work: vectors that fired and are now disabled. */
static atomic_t pending_groups;
/* Any thread -> work: re-check blocked groups and re-arm every tile. */
static atomic_t tensix_changed;

/* System work queue only. Vectors disabled and waiting for the hold-off to re-enable. */
static uint32_t masked_groups;
/* System work queue only. Vectors disabled because an asserted tile could not be cleared. */
static uint32_t blocked_groups;

static void ServiceWork(struct k_work *work);
static void RearmWork(struct k_work *work);

static K_WORK_DEFINE(service_work, ServiceWork);
static K_WORK_DELAYABLE_DEFINE(rearm_work, RearmWork);

static uint32_t SaturatingAddU32(uint32_t a, uint32_t b)
{
	if (a > UINT32_MAX - b) {
		return UINT32_MAX;
	}
	return a + b;
}

static void AddTile(const struct tensix_ecc_tile *tile)
{
	for (int src = 0; src < NOC_ECC_NUM_SOURCES; src++) {
		totals.noc[src] =
			SaturatingAddU32(totals.noc[src], tile->noc[src] & NOC_ECC_HW_CNT_MASK);
	}

	if (tile->l1_valid) {
		totals.l1_sbe = SaturatingAddU32(totals.l1_sbe, tile->l1_sbe);
		totals.l1_dbe = SaturatingAddU32(totals.l1_dbe, tile->l1_dbe);
	}
}

static void LogTile(uint8_t noc_x, uint8_t noc_y, const struct tensix_ecc_tile *tile)
{
	uint32_t noc_dbe = tile->noc[NOC_ECC_HDR_DBE] & NOC_ECC_HW_CNT_MASK;
	uint32_t noc_parity = tile->noc[NOC_ECC_MEM_PARITY] & NOC_ECC_HW_CNT_MASK;
	uint32_t l1_dbe = tile->l1_valid ? tile->l1_dbe : 0;

	if ((noc_dbe | noc_parity | l1_dbe) != 0) {
		LOG_WRN("Tensix (%u,%u) ECC: L1 DBE +%u, NOC hdr DBE +%u, NOC parity +%u", noc_x,
			noc_y, l1_dbe, noc_dbe, noc_parity);
	}
}

static bool TileFromIndex(uint32_t tensix_index, bool translation, uint8_t *noc_x, uint8_t *noc_y)
{
	uint8_t col = tensix_index / NUM_TENSIX_Y;
	uint8_t row = tensix_index % NUM_TENSIX_Y;

	if (col >= NUM_TENSIX_X || !IS_BIT_SET(tile_enable.tensix_col_enabled, col)) {
		return false;
	}

	uint8_t phys_x = TensixPhysXToNoc(col, 0);
	uint8_t phys_y = TensixPhysYToNoc(row, 0);

	if (translation) {
		NocPhysicalToLogical(phys_x, phys_y, noc_x, noc_y);
	} else {
		*noc_x = phys_x;
		*noc_y = phys_y;
	}

	return true;
}

/*
 * Service every tile asserting in one interrupt group. Caller holds NocEccStateLock with
 * NOC ECC enabled and the Tensix routers up.
 *
 * Returns true if every asserted tile was cleared, so the vector may be re-enabled.
 */
static bool ServiceGroup(uint32_t group, bool translation)
{
	uint32_t status = ReadReg(TENSIX_ERR_STATUS_ADDR(group));
	bool all_cleared = true;

	while (status != 0) {
		uint32_t bit = find_lsb_set(status) - 1;
		uint8_t noc_x;
		uint8_t noc_y;
		struct tensix_ecc_tile tile;

		status &= ~BIT(bit);

		if (!TileFromIndex(group * 32 + bit, translation, &noc_x, &noc_y)) {
			LOG_WRN("Tensix error IRQ from harvested tile index %u", group * 32 + bit);
			all_cleared = false;
			continue;
		}

		if (!TensixEccServiceTile(noc_x, noc_y, &tile)) {
			/* Gated: interrupt stays asserted until the tile is ungated. */
			all_cleared = false;
		}

		AddTile(&tile);
		LogTile(noc_x, noc_y, &tile);
	}

	return all_cleared;
}

/*
 * Collect and clear every enabled tile once. Caller holds NocEccStateLock with NOC ECC
 * enabled and the Tensix routers up.
 *
 * The NOC sources are edge-detected in the tile from the NIU's level, and the enables are
 * forced to zero while the tile is clock-gated or in reset. An error that lands then raises
 * the level, the level stays high, and no  later error on that source can make an edge.
 * Draining the NIU drops the level and accounts for what arrived while the tile could not
 * interrupt. Also catches a tile whose interrupt latch was set before the vector was enabled.
 */
static void SweepAllTiles(bool translation)
{
	for (uint32_t index = 0; index < NUM_TENSIX_X * NUM_TENSIX_Y; index++) {
		uint8_t noc_x;
		uint8_t noc_y;
		struct tensix_ecc_tile tile;

		if (!TileFromIndex(index, translation, &noc_x, &noc_y)) {
			continue;
		}

		(void)TensixEccServiceTile(noc_x, noc_y, &tile);
		AddTile(&tile);
		LogTile(noc_x, noc_y, &tile);
	}
}

static void ServiceWork(struct k_work *work)
{
	ARG_UNUSED(work);

	uint32_t groups = (uint32_t)atomic_clear(&pending_groups);
	bool changed = atomic_clear(&tensix_changed) != 0;

	if (changed) {
		groups |= blocked_groups;
	}

	NocEccStateLock();

	bool ready = NocEccEnabled() && NocTensixRoutersUp();
	bool translation = IsNocTranslationEnabled();

	/*
	 * Tiles that were gated dropped the arm write; tiles just reset came up unarmed. Arm
	 * first so an error during the sweep can still interrupt, then sweep so every NIU
	 * level is low with its counter accounted for.
	 */
	if (changed && ready) {
		TensixEccArmIrq(true, 0, 0);
		SweepAllTiles(translation);
	}

	for (uint32_t group = 0; group < TENSIX_ERR_IRQ_GROUPS; group++) {
		if (!IS_BIT_SET(groups, group)) {
			continue;
		}

		if (ready && ServiceGroup(group, translation)) {
			blocked_groups &= ~BIT(group);
			masked_groups |= BIT(group);
		} else {
			/* Retried on the next Tensix change. */
			blocked_groups |= BIT(group);
		}
	}

	NocEccStateUnlock();

	if (masked_groups != 0) {
		k_work_schedule(&rearm_work, K_MSEC(IRQ_REARM_HOLDOFF_MS));
	}
}

static void RearmWork(struct k_work *work)
{
	ARG_UNUSED(work);

	uint32_t groups = masked_groups;

	masked_groups = 0;

	for (uint32_t group = 0; group < TENSIX_ERR_IRQ_GROUPS; group++) {
		if (IS_BIT_SET(groups, group)) {
			TensixErrIrqEnable(group);
		}
	}
}

#if TENSIX_ERR_IRQ_WIRED
static void TensixErrIsr(const void *arg)
{
	uint32_t group = (uint32_t)(uintptr_t)arg;

	/* Level source: stays asserted until the work queue clears the tile. */
	TensixErrIrqDisable(group);
	atomic_or(&pending_groups, BIT(group));
	k_work_submit(&service_work);
}

#define CONNECT_TENSIX_ERR_IRQ(group, _)                                                           \
	IRQ_CONNECT(TENSIX_ERR_IRQN(group), TENSIX_ERR_IRQ_PRIO(group), TensixErrIsr,              \
		    (const void *)(uintptr_t)(group), 0)
#endif

void EccMonitorGetTotals(struct ecc_totals *out)
{
	*out = totals;
}

void EccMonitorGetIrqState(struct ecc_monitor_irq_state *out)
{
	out->running = monitor_running;
	out->pending = (uint32_t)atomic_get(&pending_groups);
	out->masked = masked_groups;
	out->blocked = blocked_groups;
	out->num_groups = TENSIX_ERR_IRQ_GROUPS;
	for (uint32_t group = 0; group < TENSIX_ERR_IRQ_GROUPS; group++) {
		out->status[group] = ReadReg(TENSIX_ERR_STATUS_ADDR(group));
	}
}

void EccMonitorTensixChanged(void)
{
	if (!monitor_running) {
		return;
	}

	atomic_set(&tensix_changed, 1);
	k_work_submit(&service_work);
}

static void TensixStateCallback(const struct zbus_channel *chan)
{
	const struct tensix_state_msg *msg = zbus_chan_const_msg(chan);

	if (msg->enable) {
		EccMonitorTensixChanged();
	}
}

ZBUS_LISTENER_DEFINE(ecc_monitor_tensix_state_listener, TensixStateCallback);
ZBUS_CHAN_ADD_OBS(tensix_state_chan, ecc_monitor_tensix_state_listener, 0);

static int ecc_monitor_init(void)
{
	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY) || !IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	if (!GetActiveFeatures().ecc) {
		return 0;
	}

	monitor_running = true;

#if TENSIX_ERR_IRQ_WIRED
	LISTIFY(TENSIX_ERR_IRQ_GROUPS, CONNECT_TENSIX_ERR_IRQ, (;));
#endif
	for (uint32_t group = 0; group < TENSIX_ERR_IRQ_GROUPS; group++) {
		TensixErrIrqEnable(group);
	}

	EccMonitorTensixChanged();

	return 0;
}
SYS_INIT_APP(ecc_monitor_init);
