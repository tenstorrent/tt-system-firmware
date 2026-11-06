/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <soc.h>
#include <tenstorrent/sep_bl1.h>

static void keraunos_post(uint32_t word)
{
	/* Postcodes on scratch[1]; scratch[0] is the SMC PASS bank. */
	WRITE_SCRATCH(1, word);
}

/* SEP BL1 only places SMC images, and their load_addr is SMC-local. */
static int keraunos_copy(uint64_t dest, const void *src, size_t len)
{
	const uint64_t sram = KER_SMC_LOCAL_BASE + (KER_SMC_SRAM_BASE - SEP_SMC_BASE);

	if (dest < sram || dest >= sram + KER_SMC_SRAM_SIZE ||
	    len > sram + KER_SMC_SRAM_SIZE - dest) {
		return -EINVAL;
	}

	memcpy((void *)(uintptr_t)KER_SMC_TO_SEP(dest), src, len);
	return 0;
}

static int keraunos_write64(uint64_t addr, uint64_t val)
{
	*(volatile uint64_t *)(uintptr_t)addr = val;
	return 0;
}

static int keraunos_read64(uint64_t addr, uint64_t *val)
{
	if (val == NULL) {
		return -EINVAL;
	}
	*val = *(volatile uint64_t *)(uintptr_t)addr;
	return 0;
}

void sep_keraunos_init_ctx(struct sep_bl1_ctx *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->hw.post_status = keraunos_post;
	ctx->hw.copy_to = keraunos_copy;
	ctx->hw.write64 = keraunos_write64;
	ctx->hw.read64 = keraunos_read64;
}
