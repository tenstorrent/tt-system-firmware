/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <tenstorrent/sep_bl1.h>
#include <zephyr/sys/util.h>

void sep_bl1_post(const struct sep_bl1_ctx *ctx, uint8_t msg_type, uint16_t value)
{
	if (ctx == NULL || ctx->hw.post_status == NULL) {
		return;
	}
	ctx->hw.post_status(sep_bl1_status_word(msg_type, value));
}

int sep_bl1_bl0_manifest_addr(const uint8_t *state_end, uint32_t *manifest_addr)
{
	uint32_t size;
	uint32_t magic;
	const uint8_t *start;

	if (state_end == NULL || manifest_addr == NULL) {
		return -EINVAL;
	}

	memcpy(&magic, state_end - 4, sizeof(magic));
	memcpy(&size, state_end - 8, sizeof(size));
	if (magic != SEP_BL1_BL0_STATE_MAGIC || size < 16U || size > 256U || (size % 4U) != 0U) {
		return -ENOENT;
	}

	start = state_end - size;
	memcpy(&magic, start, sizeof(magic));
	if (magic != SEP_BL1_BL0_STATE_MAGIC) {
		return -ENOENT;
	}

	memcpy(manifest_addr, start + 4, sizeof(*manifest_addr));
	return 0;
}

int sep_bl1_stage_bundle(const struct fw_bundle_manifest *manifest, const uint8_t *payload,
			 uint8_t *dst, size_t dst_size, const struct sep_bl1_ctx *ctx)
{
	struct fw_bundle_manifest *staged;
	uint32_t mlen;
	uint64_t plen;

	if (manifest == NULL || payload == NULL || dst == NULL) {
		return -EINVAL;
	}

	mlen = manifest->manifest_length;
	plen = manifest->payload_length;
	if (mlen < sizeof(*manifest) || mlen > dst_size || plen > dst_size - mlen) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_VALIDATE);
		return -EINVAL;
	}

	/* Payload first: its source may sit where the manifest is about to go. */
	memmove(dst + mlen, payload, (size_t)plen);
	memcpy(dst, manifest, mlen);

	staged = (struct fw_bundle_manifest *)dst;
	staged->payload_offset = (int64_t)mlen;
	return 0;
}

const struct fw_bundle_toc *sep_bl1_bundle_toc(const uint8_t *buf, size_t size)
{
	const struct fw_bundle_manifest *manifest = (const struct fw_bundle_manifest *)buf;
	const struct fw_bundle_toc *toc;
	uint64_t off;
	uint64_t count;

	if (buf == NULL || size < sizeof(*manifest) || manifest->payload_offset < 0) {
		return NULL;
	}

	off = (uint64_t)manifest->payload_offset;
	if (off > size - sizeof(*toc)) {
		return NULL;
	}

	toc = (const struct fw_bundle_toc *)(buf + off);
	count = toc->image_count;
	/* Bounding count by size first keeps the product from overflowing. */
	if (toc->toc_identifier != FW_BUNDLE_TOC_ID || count > size ||
	    count * sizeof(toc->entries[0]) > size - off - sizeof(*toc)) {
		return NULL;
	}
	return toc;
}

int sep_bl1_load_smc_bl0p5(const uint8_t *buf, size_t size, const struct sep_bl1_ctx *ctx,
			   const struct fw_bundle_toc_entry **out)
{
	const struct fw_bundle_manifest *manifest = (const struct fw_bundle_manifest *)buf;
	const struct fw_bundle_toc *toc = sep_bl1_bundle_toc(buf, size);
	const struct fw_bundle_toc_entry *img;
	const uint8_t *src;
	uint64_t off;

	if (toc == NULL || ctx == NULL) {
		return -EINVAL;
	}

	img = (toc->image_count > SEP_BL1_BUN1_SMC_BL0P5_TOC_INDEX)
		      ? &toc->entries[SEP_BL1_BUN1_SMC_BL0P5_TOC_INDEX]
		      : NULL;
	if (img == NULL || img->type != FW_BUNDLE_IMG_TYPE_SMC_BL0P5) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_MISSING_IMAGE);
		return -ENOENT;
	}

	off = (uint64_t)manifest->payload_offset;
	if (img->offset > size - off || img->length > size - off - img->offset) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_VALIDATE);
		return -EINVAL;
	}
	src = buf + off + img->offset;

	if (ctx->hw.copy_to != NULL) {
		int rc = ctx->hw.copy_to(img->load_addr, src, (size_t)img->length);

		if (rc != 0) {
			return rc;
		}
	} else {
		memcpy((void *)(uintptr_t)img->load_addr, src, (size_t)img->length);
	}

	if (out != NULL) {
		*out = img;
	}
	return 0;
}

int sep_bl1_start_smc(const struct fw_bundle_toc_entry *smc_bl0p5, const struct sep_bl1_ctx *ctx,
		      uint64_t reset_vector_addr, uint64_t reset_ctrl_addr)
{
	uint64_t ctrl;
	uint64_t entry;
	int rc;

	if (smc_bl0p5 == NULL || ctx == NULL || ctx->hw.write64 == NULL || ctx->hw.read64 == NULL) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_HW_UNAVAILABLE);
		return -ENOTSUP;
	}

	/* The TOC entry may live in SMC SRAM, which stalls while hart 0 is held. */
	entry = smc_bl0p5->entry_point;

	rc = ctx->hw.read64(reset_ctrl_addr, &ctrl);
	if (rc != 0) {
		return rc;
	}
	/* Hold hart 0 while the vector is programmed. */
	ctrl &= ~BIT64(0);
	rc = ctx->hw.write64(reset_ctrl_addr, ctrl);
	if (rc != 0) {
		return rc;
	}

	rc = ctx->hw.write64(reset_vector_addr, entry);
	if (rc != 0) {
		return rc;
	}

	ctrl |= BIT64(0);
	rc = ctx->hw.write64(reset_ctrl_addr, ctrl);
	if (rc != 0) {
		return rc;
	}

	sep_bl1_post(ctx, SEP_BL1_MSG_STATUS, SEP_BL1_STATUS_BOOT_COMPLETE);
	return 0;
}

int sep_bl1_ack_bun2_unsecured(uint32_t *scratch, const uint8_t *buf, size_t size,
			       const struct sep_bl1_ctx *ctx)
{
	if (scratch == NULL) {
		return -EINVAL;
	}
	if ((*scratch & SEP_BL1_BUNDLE_READY_FOR_VALIDATION_BIT) == 0U) {
		return 0;
	}

	if (sep_bl1_bundle_toc(buf, size) == NULL) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_VALIDATE);
		return -EINVAL;
	}

	*scratch |= SEP_BL1_BUNDLE_VALIDATED_BIT;
	return 0;
}
