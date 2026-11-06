/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * L2CPU resident stub.
 *
 * Every hart marks itself parked in the mailbox at _start + L2CPU_MAILBOX_STUB_OFFSET,
 * then sleeps in wfi, waking on its own CLINT timer about once a millisecond to poll the
 * mailbox 'go' word. When it is set, the hart clears its parked marker and jumps to
 * 'entry' with a0 = mhartid, a1 = 'arg'. mstatus.MIE is never enabled, so the timer only
 * wakes the hart and never traps.
 *
 * The ready slot and the CLINT mtimecmp register are both indexed by mhartid, which relies
 * on every cluster numbering its harts 0..3. Each L2CPU tile is a separate X280 core
 * complex with its own CLINT.
 *
 * Assembled for rv64i_zicsr_zifencei: base integer instructions only, no compressed
 * encodings, position independent. The flat binary is embedded in the SMC image and
 * written into the cluster's L3 by l2cpu_resident_start().
 */

#include "l2cpu.h"

#define CLINT_MTIMECMP 0x2004000
#define CLINT_MTIME    0x200bff8
#define MIE_MTIE       0x80
#define POLL_TICKS     0xc000	/* ~1 ms at the 50 MHz timebase */

.if (L2CPU_MAILBOX_STUB_OFFSET & 0xfff) != 0
.error "mailbox must sit at a 4 KB offset from the stub for auipc"
.endif

	.section .text
	.globl _start
_start:
	auipc	t1, L2CPU_MAILBOX_STUB_OFFSET >> 12	/* t1 = &mailbox */
	csrr	t0, mhartid
	lui	t4, %hi(CLINT_MTIMECMP)
	slli	t5, t0, 3
	add	t5, t5, t4				/* t5 = &mtimecmp[hart] */
	lui	t6, %hi(CLINT_MTIME)
	addi	t6, t6, %lo(CLINT_MTIME)		/* t6 = &mtime */
	slli	t3, t0, 2
	add	t3, t3, t1				/* t3 + READY = &ready[hart] */
	lui	t2, %hi(L2CPU_READY_MAGIC)
	addi	t2, t2, %lo(L2CPU_READY_MAGIC)
	sw	t2, L2CPU_MAILBOX_READY(t3)
	addi	a3, zero, MIE_MTIE
	csrw	mie, a3
	lui	a5, %hi(POLL_TICKS)
1:	ld	a4, 0(t6)
	add	a4, a4, a5
	sd	a4, 0(t5)				/* re-arm mtimecmp; clears MTIP */
	wfi
	lw	a2, L2CPU_MAILBOX_GO(t1)
	beqz	a2, 1b
	fence	r, rw
	ld	a0, L2CPU_MAILBOX_ENTRY(t1)
	ld	a1, L2CPU_MAILBOX_ARG(t1)
	csrw	mie, zero
	addi	a4, zero, -1
	sd	a4, 0(t5)				/* disarm the timer */
	fence
	fence.i
	addi	t2, a0, 0
	addi	a0, t0, 0				/* a0 = hartid, a1 = arg */
	sw	zero, L2CPU_MAILBOX_READY(t3)
	jr	t2
