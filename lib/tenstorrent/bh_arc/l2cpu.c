/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bh_reset.h"
#include "gddr.h"
#include "harvesting.h"
#include "init.h"
#include "l2cpu.h"
#include "noc2axi.h"
#include "noc_init.h"

#include <errno.h>

#include <tenstorrent/sys_init_defines.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/clock_control_tt_bh.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/reset/reset_tt_bh.h>
#include <zephyr/dt-bindings/reset/tt-bh-reset.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(l2cpu, CONFIG_TT_APP_LOG_LEVEL);

#define L2CPU_NOC_RING 0
#define L2CPU_NOC_TLB  0

#define NOC_TLB_WINDOW_SIZE BIT(NOC_TLB_LOG_SIZE)

/* All L2CPU tiles sit on NOC0 column 8, which translation leaves unchanged. */
#define L2CPU_NOC0_X              8
#define L2CPU_NOC0_LOGICAL_Y_BASE 26

/* Physical NOC0 Y of l2cpu_ss_inst[i] (see kL2CpuNoc0Y in noc_init.c) */
static const uint8_t kL2CpuNoc0PhysY[L2CPU_NUM_CLUSTERS] = {3, 9, 5, 7};
/* GDDR instance whose DRAM backs each cluster's aperture */
static const uint8_t kL2CpuGddrInst[L2CPU_NUM_CLUSTERS] = {5, 6, 7, 7};

/* Tile-local AXI addresses */
#define L2CPU_L3_CTRL_REG          0x02010008ULL /* CCACHE0_WAYENABLE */
#define L2CPU_L3_ENABLE_ALL        0xFU
#define L2CPU_L3_WAYMASK_BASE      0x02010800ULL /* CCACHE0_WAYMASK0[master], u64 each */
#define L2CPU_L3_WAYMASK_STRIDE    8ULL
#define L2CPU_L3_NUM_MASTERS       38
#define L2CPU_L3_NOC_MASTER_FIRST  1
#define L2CPU_L3_NOC_MASTER_LAST   32
#define L2CPU_L3_NUM_WAYS          16
#define L2CPU_L3_WAY_SIZE          (128 * 1024)
#define L2CPU_L3_WAYMASK_ALL       BIT_MASK(L2CPU_L3_NUM_WAYS)
/* L3 way that holds the resident stub and its mailbox while the cluster is parked */
#define L2CPU_L3_RESIDENT_WAY      (L2CPU_L3_NUM_WAYS - 1)
#define L2CPU_L2_PREFETCH_BASE     0x02030000ULL
#define L2CPU_L2_PREFETCH_STRIDE   0x2000ULL
#define L2CPU_L2_PREFETCH_REG0_VAL 0x15811U
#define L2CPU_L2_PREFETCH_REG1_VAL 0x38C84EU
/* RESET_VECTOR_CORE_<hart>_{0,1}: low/high 32 bits, 8 bytes per hart */
#define L2CPU_RESET_VECTOR_BASE    0xFFFFF7FEFFF10000ULL
#define L2CPU_RESET_VECTOR_STRIDE  8ULL

/* l2cpu_risc_reset_n only takes effect with L2CPUCLK below this */
#define L2CPU_RESET_TOGGLE_MAX_CLK_MHZ 300
BUILD_ASSERT(L2CPU_RESET_RELEASE_CLK_MHZ < L2CPU_RESET_TOGGLE_MAX_CLK_MHZ,
	     "L2CPU reset must be released with L2CPUCLK below 300 MHz");

static const struct device *const pll4 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(pll4));
static const struct device *const l2cpu_reset_dev =
	DEVICE_DT_GET_OR_NULL(DT_NODELABEL(l2cpu_reset));

/* Clusters released by this firmware */
static uint8_t released_mask;

uint8_t l2cpu_released_mask(void)
{
	return released_mask;
}

uint32_t l2cpu_running_gddr_mask(void)
{
	const uint8_t all_harts = BIT_MASK(L2CPU_NUM_HARTS);
	uint32_t gddr_mask = 0;

	for (uint8_t i = 0; i < L2CPU_NUM_CLUSTERS; i++) {
		if ((released_mask & BIT(i)) && l2cpu_resident_ready_mask(i) != all_harts) {
			gddr_mask |= BIT(kL2CpuGddrInst[i]);
		}
	}

	return gddr_mask;
}

void l2cpu_get_noc_coords(uint8_t l2cpu, uint8_t *x, uint8_t *y)
{
	__ASSERT_NO_MSG(l2cpu < L2CPU_NUM_CLUSTERS);

	*x = L2CPU_NOC0_X;
	*y = IsNocTranslationEnabled() ? (L2CPU_NOC0_LOGICAL_Y_BASE + l2cpu)
				       : kL2CpuNoc0PhysY[l2cpu];
}

bool l2cpu_is_enabled(uint8_t l2cpu)
{
	if (l2cpu >= L2CPU_NUM_CLUSTERS) {
		return false;
	}

	if (!(tile_enable.l2cpu_enabled & BIT(l2cpu))) {
		return false;
	}

	return (GetDramMask() & BIT(kL2CpuGddrInst[l2cpu])) != 0;
}

static void l2cpu_write32(uint8_t x, uint8_t y, uint64_t addr, uint32_t val)
{
	NOC2AXITlbSetup(L2CPU_NOC_RING, L2CPU_NOC_TLB, x, y, addr);
	NOC2AXIWrite32(L2CPU_NOC_RING, L2CPU_NOC_TLB, addr, val);
}

static uint32_t l2cpu_read32(uint8_t x, uint8_t y, uint64_t addr)
{
	NOC2AXITlbSetup(L2CPU_NOC_RING, L2CPU_NOC_TLB, x, y, addr);
	return NOC2AXIRead32(L2CPU_NOC_RING, L2CPU_NOC_TLB, addr);
}

/* Bytes left in the 16 MB window that contains @p addr */
static size_t l2cpu_window_room(uint64_t addr)
{
	return NOC_TLB_WINDOW_SIZE - (size_t)(addr & NOC_TLB_WINDOW_ADDR_MASK);
}

int l2cpu_write_image(uint8_t l2cpu, uint64_t dst, const void *src, size_t len)
{
	const uint8_t *s = src;
	uint8_t x, y;

	if (l2cpu >= L2CPU_NUM_CLUSTERS || src == NULL) {
		return -EINVAL;
	}
	if ((dst % sizeof(uint32_t)) != 0 || (len % sizeof(uint32_t)) != 0) {
		return -EINVAL;
	}

	l2cpu_get_noc_coords(l2cpu, &x, &y);

	while (len > 0) {
		size_t chunk = MIN(len, l2cpu_window_room(dst));
		volatile uint32_t *window = GetTlbWindowAddr(L2CPU_NOC_RING, L2CPU_NOC_TLB, dst);

		NOC2AXITlbSetup(L2CPU_NOC_RING, L2CPU_NOC_TLB, x, y, dst);
		for (size_t i = 0; i < chunk / sizeof(uint32_t); i++) {
			window[i] = sys_get_le32(s + i * sizeof(uint32_t));
		}

		s += chunk;
		dst += chunk;
		len -= chunk;
	}

	return 0;
}

/*
 * Set every L3 master's way mask. Masters not in [first, last] get @p others; masters in
 * that range get @p range. A way that is cleared from every master's mask is pinned: its
 * lines are never evicted to make room, only by an explicit flush.
 */
static void l2cpu_l3_set_waymasks(uint8_t x, uint8_t y, uint8_t first, uint8_t last, uint32_t range,
				  uint32_t others)
{
	for (uint8_t m = 0; m < L2CPU_L3_NUM_MASTERS; m++) {
		uint64_t reg = L2CPU_L3_WAYMASK_BASE + m * L2CPU_L3_WAYMASK_STRIDE;
		uint32_t mask = (m >= first && m <= last) ? range : others;

		l2cpu_write32(x, y, reg, mask);
		l2cpu_write32(x, y, reg + 4, 0);
	}
	(void)l2cpu_read32(x, y, L2CPU_L3_WAYMASK_BASE);
}

/*
 * Steer the NOC masters' allocations into the resident way alone and keep every other
 * master out of it. Lines the firmware writes next land in that way and nowhere else.
 */
static void l2cpu_l3_open_resident_way(uint8_t x, uint8_t y)
{
	l2cpu_write32(x, y, L2CPU_L3_CTRL_REG, L2CPU_L3_ENABLE_ALL);
	(void)l2cpu_read32(x, y, L2CPU_L3_CTRL_REG);

	l2cpu_l3_set_waymasks(x, y, L2CPU_L3_NOC_MASTER_FIRST, L2CPU_L3_NOC_MASTER_LAST,
			      BIT(L2CPU_L3_RESIDENT_WAY),
			      L2CPU_L3_WAYMASK_ALL & ~BIT(L2CPU_L3_RESIDENT_WAY));
}

/* Pin the resident way: no master may allocate into it, so its contents stay put */
static void l2cpu_l3_pin_resident_way(uint8_t x, uint8_t y)
{
	l2cpu_l3_set_waymasks(x, y, L2CPU_L3_NOC_MASTER_FIRST, L2CPU_L3_NOC_MASTER_LAST,
			      L2CPU_L3_WAYMASK_ALL & ~BIT(L2CPU_L3_RESIDENT_WAY),
			      L2CPU_L3_WAYMASK_ALL & ~BIT(L2CPU_L3_RESIDENT_WAY));
}

/* Return the resident way to the cache once nothing runs from it any more */
static void l2cpu_l3_unpin_resident_way(uint8_t x, uint8_t y)
{
	l2cpu_l3_set_waymasks(x, y, L2CPU_L3_NOC_MASTER_FIRST, L2CPU_L3_NOC_MASTER_LAST,
			      L2CPU_L3_WAYMASK_ALL, L2CPU_L3_WAYMASK_ALL);
}

static int l2cpu_set_clk(uint32_t mhz)
{
	/* The four L2CPU clocks share PLL4's VCO; re-rating one re-rates them all */
	const clock_control_subsys_t l2cpuclk =
		(clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_0;

	if (pll4 == NULL) {
		return -ENODEV;
	}

	return clock_control_set_rate(pll4, l2cpuclk, (clock_control_subsys_rate_t)mhz);
}

static int l2cpu_ensure_clk_enabled(uint8_t l2cpu)
{
	const clock_control_subsys_t l2cpuclk =
		(clock_control_subsys_t)(CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_0 + l2cpu);
	uint32_t rate;
	int rc;

	if (pll4 == NULL) {
		return -ENODEV;
	}

	rc = clock_control_get_rate(pll4, l2cpuclk, &rate);
	if (rc < 0) {
		return rc;
	}

	return (rate == 0) ? clock_control_on(pll4, l2cpuclk) : 0;
}

static int l2cpu_check_startable(uint8_t l2cpu)
{
	uint8_t in_reset;
	int rc;

	if (l2cpu >= L2CPU_NUM_CLUSTERS) {
		return -EINVAL;
	}

	if (l2cpu_reset_dev == NULL) {
		return -ENODEV;
	}

	/*
	 * The four L2CPUCLKs share PLL4's VCO, so the clock steps in l2cpu_start() would
	 * drag any running cluster down to the reset-release rate and back; starting a
	 * second cluster while one runs is not supported.
	 */
	if (released_mask != 0) {
		LOG_ERR("L2CPU%u: cluster(s) 0x%x already running; cannot start another", l2cpu,
			released_mask);
		return (released_mask & BIT(l2cpu)) ? -EALREADY : -EBUSY;
	}

	/*
	 * Re-asserting l2cpu_risc_reset_n on a released cluster neither restarts the
	 * cores nor leaves the NOC usable, so never attempt it. Only a cluster still held in
	 * reset since chip reset can be started.
	 */
	rc = reset_status(l2cpu_reset_dev, TT_BH_L2CPU_RISC_ID(l2cpu), &in_reset);
	if (rc < 0) {
		return rc;
	}
	if (!in_reset) {
		LOG_ERR("L2CPU%u: already released; restart requires a chip reset", l2cpu);
		return -EALREADY;
	}

	return 0;
}

int l2cpu_start(uint8_t l2cpu, uint64_t entry, uint32_t run_clk_mhz)
{
	uint8_t x, y;
	int rc;

	rc = l2cpu_check_startable(l2cpu);
	if (rc < 0) {
		return rc;
	}

	l2cpu_get_noc_coords(l2cpu, &x, &y);

	/* Enable the whole L3 (increase-only, so harmless if already done) */
	l2cpu_write32(x, y, L2CPU_L3_CTRL_REG, L2CPU_L3_ENABLE_ALL);
	(void)l2cpu_read32(x, y, L2CPU_L3_CTRL_REG);

	/* Every hart starts at the same entry; OpenSBI handles the hart lottery */
	for (uint8_t hart = 0; hart < L2CPU_NUM_HARTS; hart++) {
		uint64_t reg = L2CPU_RESET_VECTOR_BASE + hart * L2CPU_RESET_VECTOR_STRIDE;

		l2cpu_write32(x, y, reg, (uint32_t)entry);
		l2cpu_write32(x, y, reg + 4, (uint32_t)(entry >> 32));
	}

	rc = l2cpu_ensure_clk_enabled(l2cpu);
	if (rc < 0) {
		LOG_ERR("L2CPU%u: failed to enable L2CPUCLK: %d", l2cpu, rc);
		return rc;
	}

	/* RISC resets must be released at low speed */
	rc = l2cpu_set_clk(L2CPU_RESET_RELEASE_CLK_MHZ);
	if (rc < 0) {
		LOG_ERR("L2CPU%u: failed to set L2CPUCLK to %u MHz: %d", l2cpu,
			L2CPU_RESET_RELEASE_CLK_MHZ, rc);
		return rc;
	}

	rc = reset_tt_bh_lines_deassert(l2cpu_reset_dev, 0, BIT(TT_BH_L2CPU_RISC_ID(l2cpu)));
	if (rc < 0) {
		LOG_ERR("L2CPU%u: RISC reset deassert failed: %d", l2cpu, rc);
		return rc;
	}

	released_mask |= BIT(l2cpu);

	rc = l2cpu_set_clk(run_clk_mhz);
	if (rc < 0) {
		LOG_ERR("L2CPU%u: failed to set L2CPUCLK to %u MHz: %d", l2cpu, run_clk_mhz, rc);
		return rc;
	}

	/* L2 prefetcher tuning, per hart */
	for (uint8_t hart = 0; hart < L2CPU_NUM_HARTS; hart++) {
		uint64_t reg = L2CPU_L2_PREFETCH_BASE + hart * L2CPU_L2_PREFETCH_STRIDE;

		l2cpu_write32(x, y, reg, L2CPU_L2_PREFETCH_REG0_VAL);
		l2cpu_write32(x, y, reg + 4, L2CPU_L2_PREFETCH_REG1_VAL);
	}

	LOG_INF("L2CPU%u: released at 0x%08x%08x, L2CPUCLK %u MHz", l2cpu, (uint32_t)(entry >> 32),
		(uint32_t)entry, run_clk_mhz);

	return 0;
}

/*
 * Resident stub: the parking loop every hart runs between release and jump. Assembled
 * from l2cpu_resident.S at build time and embedded as a flat little-endian RISC-V binary.
 */
static const uint8_t l2cpu_resident_stub[] __aligned(sizeof(uint32_t)) = {
#include "l2cpu_resident.inc"
};

BUILD_ASSERT(sizeof(l2cpu_resident_stub) % sizeof(uint32_t) == 0,
	     "resident stub must be whole instructions");
BUILD_ASSERT(sizeof(l2cpu_resident_stub) <= L2CPU_MAILBOX_STUB_OFFSET,
	     "resident stub overlaps its mailbox");
BUILD_ASSERT(L2CPU_MAILBOX_BASE + L2CPU_MAILBOX_SIZE - L2CPU_RESIDENT_BASE <= L2CPU_L3_WAY_SIZE,
	     "resident stub and mailbox must fit in one L3 way");

int l2cpu_resident_start(uint8_t l2cpu, uint32_t run_clk_mhz)
{
	static const uint32_t zeros[L2CPU_MAILBOX_SIZE / sizeof(uint32_t)];
	uint8_t x, y;
	int rc;

	rc = l2cpu_check_startable(l2cpu);
	if (rc < 0) {
		return rc;
	}

	l2cpu_get_noc_coords(l2cpu, &x, &y);

	/*
	 * The zero device is cacheable, so these writes populate L3 lines rather than memory.
	 * With the masks set up first they can only allocate into the resident way, which is
	 * then pinned so nothing the harts or the host do later can evict them.
	 */
	l2cpu_l3_open_resident_way(x, y);

	rc = l2cpu_write_image(l2cpu, L2CPU_RESIDENT_BASE, l2cpu_resident_stub,
			       sizeof(l2cpu_resident_stub));
	if (rc < 0) {
		goto unpin;
	}
	rc = l2cpu_write_image(l2cpu, L2CPU_MAILBOX_BASE, zeros, sizeof(zeros));
	if (rc < 0) {
		goto unpin;
	}

	l2cpu_l3_pin_resident_way(x, y);

	/* Prove the lines are held in L3 (a miss would read the zero device as 0) */
	for (size_t off = 0; off < sizeof(l2cpu_resident_stub); off += sizeof(uint32_t)) {
		uint32_t expected = sys_get_le32(&l2cpu_resident_stub[off]);
		uint32_t word = l2cpu_read32(x, y, L2CPU_RESIDENT_BASE + off);

		if (word != expected) {
			LOG_ERR("L2CPU%u: L3 readback mismatch at +0x%zx: wrote %08x read %08x",
				l2cpu, off, expected, word);
			rc = -EIO;
			goto unpin;
		}
	}

	rc = l2cpu_start(l2cpu, L2CPU_RESIDENT_BASE, run_clk_mhz);
	if (rc < 0 && !(released_mask & BIT(l2cpu))) {
		goto unpin;
	}

	return rc;

unpin:
	/* Nothing runs from the stub: give the whole L3 back to whoever starts the cluster */
	l2cpu_l3_unpin_resident_way(x, y);
	return rc;
}

uint8_t l2cpu_resident_ready_mask(uint8_t l2cpu)
{
	const uint64_t ready = L2CPU_MAILBOX_BASE + L2CPU_MAILBOX_READY;
	uint8_t mask = 0;
	uint8_t x, y;

	if (l2cpu >= L2CPU_NUM_CLUSTERS || !(released_mask & BIT(l2cpu))) {
		return 0;
	}

	l2cpu_get_noc_coords(l2cpu, &x, &y);

	for (uint8_t hart = 0; hart < L2CPU_NUM_HARTS; hart++) {
		if (l2cpu_read32(x, y, ready + hart * sizeof(uint32_t)) == L2CPU_READY_MAGIC) {
			mask |= BIT(hart);
		}
	}

	return mask;
}

/*
 * Park one cluster in the resident stub: the lowest-numbered one that is not harvested and
 * whose GDDR is usable. The stub lives in the cluster's L3, so parking needs nothing from
 * the GDDR. Whatever runs on it later is loaded into DRAM by the host, which enters it
 * through the stub's mailbox (see l2cpu_resident.S); that avoids the one-shot cluster
 * reset. The host unpins the stub's L3 way once every hart has left.
 */
static int l2cpu_boot(void)
{
	uint8_t l2cpu = 0;
	int rc;

	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY) || !IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	if (is_cable_fault_mode()) {
		return 0;
	}

	while (l2cpu < L2CPU_NUM_CLUSTERS && !l2cpu_is_enabled(l2cpu)) {
		l2cpu++;
	}
	if (l2cpu == L2CPU_NUM_CLUSTERS) {
		LOG_WRN("No usable L2CPU cluster (all harvested or GDDR masked)");
		return 0;
	}

	rc = l2cpu_resident_start(l2cpu, L2CPU_RUN_CLK_MHZ);
	if (rc < 0) {
		record_init_failure(INIT_STAGE_L2CPU_BOOT);
	}

	return 0;
}

SYS_INIT_APP(l2cpu_boot);
