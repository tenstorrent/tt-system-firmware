/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * rom_start is the first section in the ROM group, ahead of Zephyr's reset
 * vector, so this puts the trampoline where a core entering at the image base
 * lands. KEEP because nothing references it: it is reached by being first.
 */
KEEP(*(.boot_trampoline))
