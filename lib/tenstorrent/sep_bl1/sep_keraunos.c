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

static int keraunos_copy(uint64_t dest, const void *src, size_t len)
{
	memcpy((void *)(uintptr_t)dest, src, len);
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
	ctx->device.secure_boot = false;
	ctx->device.lifecycle.f.test_dev = 1;
	ctx->device.allowed_key_sel_mask = 0xFF;
	ctx->hw.post_status = keraunos_post;
	ctx->hw.copy_to = keraunos_copy;
	ctx->hw.write64 = keraunos_write64;
	ctx->hw.read64 = keraunos_read64;
}
