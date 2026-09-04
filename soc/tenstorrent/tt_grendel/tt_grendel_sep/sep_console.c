/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 *
 * SEP virtual console. Testbenches decode 32-bit writes to the SEP console
 * scratch register rather than a UART, so printk() is routed here.
 *
 * Wire format, matching tt_sep firmware/common/lib/virt_console.c:
 *   [31:8] payload, low payload byte is the first character
 *   [7:4]  reserved, zero
 *   [3:1]  opcode, 0 for ASCII
 *   [0]    toggle, flipped when a write would otherwise repeat the previous
 *          value so the monitor still sees a new write
 */

#include <stdbool.h>
#include <stdint.h>

#include <soc.h>

#define CONSOLE_OP_ASCII (0x0U << 1)

/* Payload byte index: 1 is the first character, 3 the last. */
#define PAYLOAD_FIRST 1
#define PAYLOAD_END   4

static void console_write(uint32_t val)
{
	static uint32_t prev;

	if (val == prev) {
		val ^= 1U;
	}
	WRITE_SEP_SCRATCH(SEP_SCRATCH_CONSOLE, val);
	prev = val;
}

int arch_printk_char_out(int c)
{
	static uint32_t packed = CONSOLE_OP_ASCII;
	static int offset = PAYLOAD_FIRST;

	packed |= (uint32_t)(c & 0xFF) << (8 * offset);
	offset++;

	/* Flush a full word, and flush short on newline so partial lines are
	 * not held back waiting for a third character.
	 */
	if (offset == PAYLOAD_END || c == '\n') {
		console_write(packed);
		packed = CONSOLE_OP_ASCII;
		offset = PAYLOAD_FIRST;
	}

	return 0;
}

void sep_console_verdict(bool pass)
{
	WRITE_SEP_SCRATCH(SEP_SCRATCH_VERDICT, pass ? SEP_TEST_PASS_CODE : SEP_TEST_FAIL_CODE);
}
