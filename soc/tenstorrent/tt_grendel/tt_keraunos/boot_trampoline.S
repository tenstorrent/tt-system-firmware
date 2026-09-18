/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Re-anchor the program counter into the core-local SRAM alias before any
 * PC-relative addressing runs.
 *
 * Keraunos SRAM answers at two addresses: the core-local alias the image is
 * linked at, 0xc0060000, and the SoC-global view of the same memory at
 * 0x08060000. Hosts are free to start the core at either, and the one that
 * drives the two-die D2D runs starts it at the global view -- it takes the
 * vector from the chiplet-local address of the SPM rather than from a
 * constant, which is what Mimir gets.
 *
 * Booting there very nearly works. Code fetch is happy and so are ordinary
 * loads and stores, because the two views are the same memory. Atomics are
 * not: the SMC Rocket core does not implement AMOs against the global alias,
 * so the first one traps. Nothing in early boot announces itself as an atomic
 * either -- it arrives inside a logging or kernel lock long after the address
 * the core started at has stopped being obvious -- which is why this is dealt
 * with in the first instruction rather than diagnosed later.
 *
 * Mimir has the same pair of views but no equivalent of this: every host that
 * starts a Mimir SMC core uses the core-local alias already.
 *
 * The technique is the one the Sival drop's entry.S uses, and the awkwardness
 * is not decoration. The jump target has to be a linker-resolved absolute
 * address, so it cannot be computed from the program counter, and the two
 * obvious ways of getting one into a register do not survive: `li t0, label`
 * is rejected outright, as the assembler cannot choose an expansion for a
 * value it does not yet know, and `lui`+`addi` of %hi/%lo sign-extends an
 * address with bit 31 set into 0xffffffff_c006xxxx, which faults on the jump.
 * A literal pool sidesteps both. `auipc`+`ld` reaches it PC-relatively, which
 * works from either alias, while the value loaded is whatever the linker
 * wrote: the absolute, always-local address.
 */

#include <zephyr/toolchain.h>

/*
 * Placed at the very start of the image by boot_trampoline.ld, ahead of
 * Zephyr's own reset vector, so it is what a core entering at the image base
 * runs first.
 */
.section .boot_trampoline, "ax"
.global __tt_keraunos_boot_trampoline
__tt_keraunos_boot_trampoline:
.Lanchor:
	auipc t0, %pcrel_hi(.Llanding_addr)
	ld t0, %pcrel_lo(.Lanchor)(t0)
	jr t0

	.balign 8
.Llanding_addr:
	/*
	 * __start, the entry this displaced, rather than anything further in:
	 * the first thing it does is establish the global pointer, and the
	 * trap vector follows. Both are PC-relative, which is the whole reason
	 * for arriving here first, and neither happens anywhere else.
	 *
	 * Jumped to rather than fallen through into. __start is in its own
	 * output section, and whatever padding the linker puts between the two
	 * would otherwise be executed as instructions.
	 */
	.dword __start
