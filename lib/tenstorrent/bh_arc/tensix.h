/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TENSIX_H
#define TENSIX_H

#include <stdbool.h>
#include <stdint.h>

#define TENSIX_INSTRUCTION_UNPACR 0x42800091u

/*
 * RMWCIBn: read-modify-write byte n of Tensix config register @p cfg_addr (word index) with
 * @p mask / @p data, executed by the Tensix itself.
 */
#define TENSIX_INSTRUCTION_OP(opcode, params) (((uint32_t)(opcode) << 24) | (uint32_t)(params))
#define TENSIX_INSTRUCTION_RMWCIB(n, mask, data, cfg_addr)                                         \
	TENSIX_INSTRUCTION_OP(0xB3u + (n), (((mask) & 0xFFu) << 16) | (((data) & 0xFFu) << 8) |    \
						   ((cfg_addr) & 0xFFu))

/**
 * @brief Inject one instruction into a Tensix tile via the RISC debug instruction buffer
 *
 * @param instruction  Raw Tensix instructions.
 * @param thread       Thread index [0,3].
 * @param broadcast    If true, use the Tensix multicast TLB so every non-harvested Tensix
 *                         receives the sequence. If false, unicast to (noc_x, noc_y) only.
 * @param noc_x        X coordinate of the NOC tile when @p broadcast is false.
 *                         Ignored when broadcasting.
 * @param noc_y        Y coordinate of the NOC tile when @p broadcast is false.
 *                         Ignored when broadcasting.
 */
void tensix_inject_instruction(uint32_t instruction, uint8_t thread, bool broadcast, uint8_t noc_x,
			       uint8_t noc_y);

#endif /* TENSIX_H */
