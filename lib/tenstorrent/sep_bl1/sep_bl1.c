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

static int apply_bl1_side_effects(const struct fw_bundle_manifest *m, const struct sep_bl1_ctx *ctx)
{
	int rc;

	if (m->authenticated_flags.f.security_version_update) {
		if (ctx->hw.update_security_version == NULL) {
			return -ENOTSUP;
		}
		rc = ctx->hw.update_security_version(m->security_version);
		if (rc != 0) {
			return rc;
		}
	}

	if (m->authenticated_flags.f.bl1_demotion) {
		if (ctx->hw.lock_demotion == NULL) {
			return -ENOTSUP;
		}
		rc = ctx->hw.lock_demotion();
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

int sep_bl1_validate_staged_bundle(const uint8_t *buf, size_t size, const struct sep_bl1_ctx *ctx,
				   struct tt_fw_bundle_view *view,
				   const struct fw_bundle_toc_entry **smc_bl1,
				   const struct fw_bundle_toc_entry **sep_mis)
{
	int rc;
	const struct fw_bundle_toc_entry *smc;
	const struct fw_bundle_toc_entry *mis;

	if (ctx == NULL || view == NULL) {
		return -EINVAL;
	}

	sep_bl1_post(ctx, SEP_BL1_MSG_STATUS, SEP_BL1_STATUS_BOOT_START);

	rc = tt_fw_bundle_validate(buf, size, &ctx->device, &ctx->auth, view);
	if (rc == -EBADMSG) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_HASH);
		return rc;
	}
	if (rc == -EPERM) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_POLICY);
		return rc;
	}
	if (rc == -ENOTSUP) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_HW_UNAVAILABLE);
		return rc;
	}
	if (rc != 0) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_VALIDATE);
		return rc;
	}

	rc = apply_bl1_side_effects(view->manifest, ctx);
	if (rc != 0) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_HW_UNAVAILABLE);
		return rc;
	}

	smc = tt_fw_bundle_find_image(view, FW_BUNDLE_IMG_TYPE_SMC_BL1);
	mis = tt_fw_bundle_find_image(view, FW_BUNDLE_IMG_TYPE_SEP_MIS);

	if (smc_bl1 != NULL) {
		*smc_bl1 = smc;
	}
	if (sep_mis != NULL) {
		*sep_mis = mis;
	}

	if (smc == NULL) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_MISSING_IMAGE);
		return -ENOENT;
	}

	return 0;
}

int sep_bl1_service_validation_scratch(uint32_t *scratch, const uint8_t *buf, size_t size,
				       const struct sep_bl1_ctx *ctx,
				       struct tt_fw_bundle_view *view)
{
	int rc;
	const struct fw_bundle_toc_entry *smc_bl1 = NULL;
	const struct fw_bundle_toc_entry *sep_mis = NULL;

	if (scratch == NULL) {
		return -EINVAL;
	}
	if ((*scratch & SEP_BL1_BUNDLE_READY_FOR_VALIDATION_BIT) == 0U) {
		return 0;
	}

	rc = sep_bl1_validate_staged_bundle(buf, size, ctx, view, &smc_bl1, &sep_mis);
	if (rc != 0) {
		return rc;
	}

	*scratch |= SEP_BL1_BUNDLE_VALIDATED_BIT;
	sep_bl1_post(ctx, SEP_BL1_MSG_STATUS, SEP_BL1_STATUS_BOOT_COMPLETE);
	return 0;
}

int sep_bl1_enter_mission_firmware(const struct tt_fw_bundle_view *view,
				   const struct sep_bl1_ctx *ctx)
{
	const struct fw_bundle_toc_entry *mis;
	uint64_t entry;
	int rc;

	if (view == NULL || ctx == NULL) {
		return -EINVAL;
	}

	mis = tt_fw_bundle_find_image(view, FW_BUNDLE_IMG_TYPE_SEP_MIS);
	if (mis == NULL) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_MISSING_IMAGE);
		return -ENOENT;
	}

	entry = tt_fw_bundle_entry_point(mis);
	if (ctx->hw.validate_sep_entry == NULL) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_HW_UNAVAILABLE);
		return -ENOTSUP;
	}
	rc = ctx->hw.validate_sep_entry(mis->load_addr, entry, mis->length);
	if (rc != 0) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_ENTRY_POINT);
		return rc;
	}

	if (ctx->hw.enter_mission == NULL) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_HW_UNAVAILABLE);
		return -ENOTSUP;
	}

	rc = ctx->hw.enter_mission(entry);
	sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_STATUS_UNEXPECTED_EXIT);
	return (rc != 0) ? rc : -EIO;
}

int sep_bl1_copy_image(const struct tt_fw_bundle_view *view,
		       const struct fw_bundle_toc_entry *entry, const struct sep_bl1_ctx *ctx)
{
	const uint8_t *src;

	if (view == NULL || entry == NULL || ctx == NULL) {
		return -EINVAL;
	}

	src = tt_fw_bundle_image_data(view, entry);
	if (src == NULL) {
		return -EINVAL;
	}

	if (ctx->hw.copy_to != NULL) {
		return ctx->hw.copy_to(entry->load_addr, src, (size_t)entry->length);
	}

	memcpy((void *)(uintptr_t)entry->load_addr, src, (size_t)entry->length);
	return 0;
}

int sep_bl1_load_smc_bl0p5(const struct tt_fw_bundle_view *view, const struct sep_bl1_ctx *ctx,
			   const struct fw_bundle_toc_entry **out)
{
	const struct fw_bundle_toc_entry *img;
	int rc;

	img = tt_fw_bundle_find_image(view, FW_BUNDLE_IMG_TYPE_SMC_BL0P5);
	if (img == NULL) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_MISSING_IMAGE);
		return -ENOENT;
	}

	rc = sep_bl1_copy_image(view, img, ctx);
	if (rc != 0) {
		return rc;
	}

	if (out != NULL) {
		*out = img;
	}
	return 0;
}

int sep_bl1_place_serdes(const struct tt_fw_bundle_view *view, const struct sep_bl1_ctx *ctx,
			 uint64_t sram_base, uint64_t sram_size, int *placed)
{
	uint64_t i;
	int n = 0;
	int rc;

	if (view == NULL || view->toc == NULL || ctx == NULL) {
		return -EINVAL;
	}

	for (i = 0; i < view->toc->image_count; i++) {
		const struct fw_bundle_toc_entry *e = &view->toc->entries[i];

		if (e->load_addr < sram_base || e->load_addr >= sram_base + sram_size) {
			continue;
		}
		if (e->length > sram_size - (e->load_addr - sram_base)) {
			return -EINVAL;
		}
		rc = sep_bl1_copy_image(view, e, ctx);
		if (rc != 0) {
			return rc;
		}
		n++;
	}

	if (placed != NULL) {
		*placed = n;
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

	entry = tt_fw_bundle_entry_point(smc_bl0p5);

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
			       const struct sep_bl1_ctx *ctx, struct tt_fw_bundle_view *view)
{
	int rc;

	if (scratch == NULL) {
		return -EINVAL;
	}
	if ((*scratch & SEP_BL1_BUNDLE_READY_FOR_VALIDATION_BIT) == 0U) {
		return 0;
	}

	rc = tt_fw_bundle_parse(buf, size, view);
	if (rc != 0) {
		sep_bl1_post(ctx, SEP_BL1_MSG_ERROR, SEP_BL1_ERROR_VALIDATE);
		return rc;
	}

	*scratch |= SEP_BL1_BUNDLE_VALIDATED_BIT;
	return 0;
}
