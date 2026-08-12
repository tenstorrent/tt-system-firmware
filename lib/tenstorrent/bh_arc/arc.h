/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARC_H
#define ARC_H

#include <tenstorrent/reg.h>
#include "status_reg.h"

#include <stdint.h>

#include <zephyr/sys/util_macro.h>

#define ARC_AUX_TIMER_0_COUNT   (0x21)
#define ARC_AUX_TIMER_0_CONTROL (0x22)
#define ARC_AUX_TIMER_0_LIMIT   (0x23)

#define ARC_CSM_START_ADDR  (0x10000000)
#define ARC_ICCM_START_ADDR (0x00000000)

#define ARC_AUX_INT_VECTOR_BASE (0x25)

#define ARC_ICAUSE           (0x40a)
#define ARC_IRQ_SELECT       (0x40b)
#define ARC_IRQ_ENABLE       (0x40c)
#define ARC_IRQ_TRIGGER      (0x40d)
#define ARC_IRQ_PULSE_CANCEL (0x415)
#define ARC_IRQ_PRIORITY     (0x206)

static inline unsigned int ArcGetTimer0(void)
{
	unsigned volatile int count;

	if (!IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	/*LR r1,[r2] ; Load contents of Aux. register ;pointed ; to by r2 into r1 */
	__asm__ __volatile__("mov r1, %[addr]\n"
			     "lr  %[reg], [r1]\n"
			     : [reg] "=r"(count)
			     : [addr] "I"(ARC_AUX_TIMER_0_COUNT)
			     : "r1");
	return count;
}

static inline void ArcWriteAux(unsigned int addr, unsigned int value)
{
	if (!IS_ENABLED(CONFIG_ARC)) {
		return;
	}

	/*SR r1,[r2] ; Store contents of r1 into Aux. register pointed to by r2  */
	__asm__ __volatile__("mov r1, %[addr]\n"
			     "sr %[reg], [r1]\n"
			     :
			     : [reg] "r"(value), [addr] "r"(addr)
			     : "r1");
}

static inline unsigned int ArcReadAux(unsigned int addr)
{
	unsigned volatile int value;

	if (!IS_ENABLED(CONFIG_ARC)) {
		return 0;
	}

	/*LR r1,[r2] ; Load contents of Aux. register ;pointed ; to by r2 into r1 */
	__asm__ __volatile__("mov r1, %[addr]\n"
			     "lr  %[reg], [r1]\n"
			     : [reg] "=r"(value)
			     : [addr] "r"(addr)
			     : "r1");
	return value;
}

static inline void _clri(void)
{
	if (!IS_ENABLED(CONFIG_ARC)) {
		return;
	}

	/*clri; disables the interrupts clears any pending. */
	__asm__ __volatile__("clri\n"
			     :      /*no output */
			     :      /*no input */
			     : "cc" /*no regs other than status32. */
	);
}

static inline void _rtie(void)
{
	if (!IS_ENABLED(CONFIG_ARC)) {
		return;
	}

	__asm__ __volatile__("rtie\n"
			     :      /*no output */
			     :      /*no input */
			     : "cc" /* status32 clobbered. */
	);
}

static inline void _seti(unsigned int flags)
{
	if (!IS_ENABLED(CONFIG_ARC)) {
		return;
	}

	/*seti r1; Sets the status register interrupt enable and level. */
	__asm__ __volatile__("mov  r1, %[reg]\n"
			     "seti r1\n"
			     : /*no output */
			     : [reg] "r"(flags)
			     : "r1", "cc" /*clobbered */
	);
}

static inline void ArcDumpIsrVects(void)
{
	uint32_t volatile *p_base_isr_vect;

	p_base_isr_vect = (uint32_t *)(ArcReadAux(ARC_AUX_INT_VECTOR_BASE));

	for (uint32_t i = 0; i < 256; i++) {
		WriteReg(RESET_UNIT_SCRATCH_REG_ADDR(6), i);
		WriteReg(RESET_UNIT_SCRATCH_REG_ADDR(7), p_base_isr_vect[i]);
	}
}

static inline void ArcSetIsrVect(uint32_t volatile intvec, volatile uint32_t intvec_num)
{
	/*# intvbase_preset --- This sets the upper 22 bits of the interrupt vector base
	 * configuration
	 */
	/* register, VECBASE_AC_BUILD.  On reset, that register is loaded into the interrupt vector
	 * base
	 */
	/* address register, INT_VECTOR_BASE.  Because this value is the upper 22 bits,  */
	/* the vector base is aligned to a 1K-byte boundary. */
	/* -intvbase_preset 0x20_0000 */
	uint32_t volatile *p_reg;

	p_reg = (uint32_t volatile *)(ArcReadAux(ARC_AUX_INT_VECTOR_BASE));

	uint32_t volatile temp = intvec_num;

	p_reg[temp] = intvec;
	/* Reference only.
	 * __asm__ __volatile__ (
	 *      "mov r1 %[intv]\n" // get the vec pointer in r1
	 *      "mov r2,[r1]\n"    // mov the pointer content into r2
	 *
	 *      "mov r1,%[addr]\n"  // mov INT_VECTOR_BASE addr into r1
	 *      "lr r3,[r1]\n"      // load from aux INT_VECTOR_BASE addr into r1
	 *
	 *      "st r2,[r3,%[offset]]\n" // store the new value into the vector location.
	 *      : // no output
	 *      : [intv] "r" (intvec),
	 *        [offset] "r" (intvec_num),
	 *        [addr] "I" (ARC_AUX_INT_VECTOR_BASE)
	 *      : "r1", "r2", "r3", "cc"
	 *      );
	 */
}

static inline void ArcSleep(void)
{
	if (!IS_ENABLED(CONFIG_ARC)) {
		return;
	}

	__asm__ __volatile__("sleep");
}
#endif
