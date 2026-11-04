/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LIB_TENSTORRENT_BH_ARC_L2CPU_H_
#define LIB_TENSTORRENT_BH_ARC_L2CPU_H_

/* Also included by l2cpu_resident.S: keep the constants below assembler-friendly */
#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#endif /* __ASSEMBLER__ */

#define L2CPU_NUM_CLUSTERS 4
#define L2CPU_NUM_HARTS    4

/*
 * Resident stub. At boot the firmware places a small program in the cluster's L3 "zero
 * device" window, releases the cluster, and leaves every hart parked in wfi polling the
 * mailbox below. Payloads (OpenSBI, device tree, kernel) are loaded into the cluster's
 * DRAM by the host (scripts/l2cpu_boot.py), which owns that layout; the firmware never
 * touches the DRAM itself.
 */
#define L2CPU_ZERO_DEVICE_BASE    0x0A000000ULL
#define L2CPU_RESIDENT_BASE       L2CPU_ZERO_DEVICE_BASE
#define L2CPU_MAILBOX_STUB_OFFSET 0x1000 /* mailbox relative to the stub's _start */
#define L2CPU_MAILBOX_BASE        (L2CPU_RESIDENT_BASE + L2CPU_MAILBOX_STUB_OFFSET)

#define L2CPU_MAILBOX_GO    0x00 /* u32: nonzero -> jump */
#define L2CPU_MAILBOX_ENTRY 0x08 /* u64: jump target */
#define L2CPU_MAILBOX_ARG   0x10 /* u64: passed in a1 */
#define L2CPU_MAILBOX_READY 0x18 /* u32[L2CPU_NUM_HARTS]: L2CPU_READY_MAGIC while parked */
#define L2CPU_MAILBOX_SIZE  0x28
#define L2CPU_READY_MAGIC   0x50415243 /* "PARC" */

/*
 * L2CPUCLK while releasing the harts from reset (must be below 300 MHz) and once running
 * (1750 MHz is the highest rate validated on silicon).
 */
#define L2CPU_RESET_RELEASE_CLK_MHZ 200
#define L2CPU_RUN_CLK_MHZ           1750

#ifndef __ASSEMBLER__

/**
 * @brief NOC0 coordinates of an L2CPU tile, in whatever coordinate space the ARC NIU is
 * currently using (logical if NOC translation is enabled, otherwise physical).
 */
void l2cpu_get_noc_coords(uint8_t l2cpu, uint8_t *x, uint8_t *y);

/**
 * @brief True if the cluster and the GDDR instance that backs its DRAM aperture are both
 * unharvested and not masked out.
 */
bool l2cpu_is_enabled(uint8_t l2cpu);

/**
 * @brief Copy @p len bytes from ARC memory into the L2CPU address space at @p dst.
 *
 * Handles re-aiming the 16 MB NOC2AXI window as needed. @p dst and @p len should be
 * 4-byte aligned.
 */
int l2cpu_write_image(uint8_t l2cpu, uint64_t dst, const void *src, size_t len);

/**
 * @brief Place the resident stub at L2CPU_RESIDENT_BASE in a pinned L3 way, clear its
 * mailbox, verify the code read back through the NOC, and release the cluster via
 * l2cpu_start().
 *
 * @return 0 on success, -EIO if the readback fails, or the error from
 * l2cpu_write_image()/l2cpu_start() (including -EALREADY).
 */
int l2cpu_resident_start(uint8_t l2cpu, uint32_t run_clk_mhz);

/**
 * @brief Bitmask of harts currently parked in the resident stub (one bit per hart).
 *
 * Returns 0 if the cluster was never released, or once the harts have left the stub.
 */
uint8_t l2cpu_resident_ready_mask(uint8_t l2cpu);

/**
 * @brief Release an L2CPU cluster from reset with all harts starting at @p entry.
 *
 * Sequence: enable full L3, program the four hart reset vectors, drop L2CPUCLK to
 * L2CPU_RESET_RELEASE_CLK_MHZ, deassert the cluster's RISC reset line, ramp L2CPUCLK to
 * @p run_clk_mhz, then program the L2 prefetchers. Payloads must already be in place.
 *
 * One-shot per chip reset: the cluster-level reset line cannot restart a cluster that has
 * already been released, so this returns -EALREADY if the cluster is not currently
 * held in reset. The four harts always start together. Returns -EBUSY if another
 * cluster is already running: the four L2CPUCLKs share PLL4's VCO, so the clock steps
 * here would drag it through the reset-release rate.
 */
int l2cpu_start(uint8_t l2cpu, uint64_t entry, uint32_t run_clk_mhz);

/**
 * @brief Bitmask of clusters this firmware has released from reset via l2cpu_start().
 *
 * While non-zero, PLL4 must not be power-cycled: all four L2CPUCLKs share its VCO, so
 * gating or re-locking it stalls the running harts mid-transaction, and the cluster cannot
 * be restarted afterwards. bh_set_l2cpu_enable() consults this.
 */
uint8_t l2cpu_released_mask(void);

#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLER__ */

#endif /* LIB_TENSTORRENT_BH_ARC_L2CPU_H_ */
