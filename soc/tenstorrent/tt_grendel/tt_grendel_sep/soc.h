/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SOC_TENSTORRENT_TT_GRENDEL_TT_GRENDEL_SEP_SOC_H_
#define SOC_TENSTORRENT_TT_GRENDEL_TT_GRENDEL_SEP_SOC_H_

#include <stdbool.h>
#include <stdint.h>

/* ICCM: ROM holding BL0, then the SRAM ("IRAM") that BL1 executes from. */
#define SEP_ICCM_ROM_BASE 0x70000000UL
#define SEP_ICCM_ROM_SIZE 0x00010000UL
#define SEP_IRAM_BASE     0x70010000UL
#define SEP_IRAM_SIZE     0x00020000UL

#define SEP_DCCM_BASE 0x80000000UL
#define SEP_DCCM_SIZE 0x00010000UL

/* BL0 leaves its handoff struct at the end of DCCM, so BL1 can walk back from
 * the magic. The devicetree dccm node stops short of it.
 */
#define SEP_BL0_STATE_RESERVED 0x80U
#define SEP_BL0_STATE_END      (SEP_DCCM_BASE + SEP_DCCM_SIZE)

/* SEP's own resources are aliased above this base; the SEP SRAM scratch pad
 * shared with the rest of the system sits inside that window.
 */
#define SEP_EXT_BASE  0xC0000000UL
#define SEP_SRAM_BASE (SEP_EXT_BASE + 0x01800000UL)
#define SEP_SRAM_SIZE 0x00010000UL

/* SEP's own cold-reset scratch bank, 8-byte stride. SEP firmware uses
 * scratch 0 for the pass/fail verdict, 1 for status, and 2 for the virtual
 * console the testbenches decode.
 */
#define SEP_SCRATCH_COLD_BASE (SEP_EXT_BASE + 0x0111A000UL)

#define WRITE_SEP_SCRATCH(num, val)                                                                \
	(*((volatile uint32_t *)(SEP_SCRATCH_COLD_BASE + ((unsigned int)(num) * 8U))) = (val))

#define SEP_SCRATCH_VERDICT 0U
#define SEP_SCRATCH_STATUS  1U
#define SEP_SCRATCH_CONSOLE 2U

#define SEP_TEST_PASS_CODE 0xACAFACA1U
#define SEP_TEST_FAIL_CODE 0xDEADBEEFU

/* SMC seen from the SEP. The base is programmable; this is the reset value of
 * SEP_CPU_CTRL_SMC_LOCAL_BASE_ADDR. Offsets match the SMC-local addresses
 * app/bl0p5 uses from its own base.
 *
 * Reading the register is more robust than trusting this constant, since
 * software may reprogram the window at early boot.
 */
#define SEP_SMC_LOCAL_BASE_ADDR_REG (SEP_EXT_BASE + 0x011100E8UL)
#define SEP_SMC_BASE                0xC2000000UL

#define KER_SMC_RESET_VECTOR0_ADDR     (SEP_SMC_BASE + 0x00010000UL)
#define KER_SMC_RESET_CTRL_ADDR        (SEP_SMC_BASE + 0x00010020UL)
#define KER_SMC_SCRATCH_BASE           (SEP_SMC_BASE + 0x00010100UL)
#define KER_SMC_BUNDLE_VALIDATION_ADDR (SEP_SMC_BASE + 0x00010150UL)
#define KER_SMC_HOST_BOOT_STATE_ADDR   (SEP_SMC_BASE + 0x00010160UL)

#define KER_SMC_SRAM_BASE       (SEP_SMC_BASE + 0x00060000UL)
#define KER_SMC_SRAM_SIZE       0x00100000UL
#define KER_SMC_BL0P5_LOAD_ADDR (SEP_SMC_BASE + 0x00150000UL)

/* BUN1 and BUN2 share one window, up to BL0P5's own SRAM. This is the same
 * span the SMC opens to the host as BUN2 staging.
 */
#define KER_BUNDLE_STAGING_ADDR (SEP_SMC_BASE + 0x00066400UL)
#define KER_BUNDLE_STAGING_SIZE (KER_SMC_BL0P5_LOAD_ADDR - KER_BUNDLE_STAGING_ADDR)

/* SMC TOC load_addr and entry_point are in the SMC's own address space, where
 * its block starts here. The SEP reaches the same block at SEP_SMC_BASE.
 */
#define KER_SMC_LOCAL_BASE   0xC0000000UL
#define KER_SMC_TO_SEP(addr) (SEP_SMC_BASE + ((addr) - KER_SMC_LOCAL_BASE))
#define KER_SMC_BL0P5_ENTRY  (KER_SMC_LOCAL_BASE + 0x00150000UL)

#define KER_SMC_RESET_CTRL_CORE0_RESET_N (1ULL << 0)

#define WRITE_SMC_SCRATCH(num, val)                                                                \
	(*((volatile uint32_t *)(KER_SMC_SCRATCH_BASE + ((unsigned int)(num) * 8U))) = (val))

/* On the SEP, an unqualified scratch write means the SEP's own bank. */
#define WRITE_SCRATCH(num, val) WRITE_SEP_SCRATCH(num, val)

/** @brief Publish the pass/fail verdict the testbenches poll. */
void sep_console_verdict(bool pass);

#endif /* SOC_TENSTORRENT_TT_GRENDEL_TT_GRENDEL_SEP_SOC_H_ */
