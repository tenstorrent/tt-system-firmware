/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TENSTORRENT_SEP_BL1_H_
#define TENSTORRENT_SEP_BL1_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Status word: [31:24] message type, [23:16] firmware id, [15:0] value.
 * SEP BL1 firmware id is 2.
 */
#define SEP_BL1_MSG_STATUS 0x01U
#define SEP_BL1_MSG_ERROR  0x0FU

#define SEP_BL1_FW_ID 0x02U

#define SEP_BL1_STATUS_BOOT_START    0x0010U
#define SEP_BL1_STATUS_BOOT_COMPLETE 0x0050U
#define SEP_BL1_ERROR_VALIDATE       0x0140U
#define SEP_BL1_ERROR_MISSING_IMAGE  0x0142U
#define SEP_BL1_ERROR_HW_UNAVAILABLE 0x0146U

/**
 * BUN2 validation handshake bits. Must stay identical to SMC BL0P5
 * (app/bl0p5/src/main.c): bit0 = ready for validation, bit1 = validated.
 */
#define SEP_BL1_BUNDLE_READY_FOR_VALIDATION_BIT BIT(0)
#define SEP_BL1_BUNDLE_VALIDATED_BIT            BIT(1)

/**
 * Bundle staging handshake, in the host-boot-state scratch register. Must stay
 * identical to SMC BL0P5 (app/bl0p5/src/main.c), which blocks on BUNDLE_STAGED
 * before it ever raises the validation doorbell, and reports BUNDLE_CONSUMED
 * once it has taken SMC BL1 out of the staging area.
 */
#define SEP_BL1_HOST_BOOT_STATE_MASK            0xFU
#define SEP_BL1_HOST_BOOT_STATE_WAIT_FOR_BUNDLE 1U
#define SEP_BL1_HOST_BOOT_STATE_BUNDLE_STAGED   2U
#define SEP_BL1_HOST_BOOT_STATE_BUNDLE_CONSUMED 3U

#define SEP_BL1_STATUS_EXTRACT_MSG_TYPE(v) ((uint8_t)(((v) >> 24) & 0xFF))
#define SEP_BL1_STATUS_EXTRACT_FW_ID(v)    ((uint8_t)(((v) >> 16) & 0xFF))
#define SEP_BL1_STATUS_EXTRACT_VALUE(v)    ((uint16_t)((v) & 0xFFFF))

/**
 * @brief Platform operations. NULL required ops fail with -ENOTSUP.
 */
struct sep_bl1_hw_ops {
	/** @brief Publish a packed status word, or NULL if unimplemented */
	void (*post_status)(uint32_t word);
	/**
	 * @brief Copy @a len bytes to a possibly remote destination.
	 * If NULL, memcpy() is used (same address space).
	 */
	int (*copy_to)(uint64_t dest, const void *src, size_t len);
	/** @brief 64-bit MMIO write, required to start SMC */
	int (*write64)(uint64_t addr, uint64_t val);
	/** @brief 64-bit MMIO read, required to start SMC */
	int (*read64)(uint64_t addr, uint64_t *val);
};

struct sep_bl1_ctx {
	struct sep_bl1_hw_ops hw;
};

static inline uint32_t sep_bl1_status_word(uint8_t msg_type, uint16_t value)
{
	return ((uint32_t)msg_type << 24) | ((uint32_t)SEP_BL1_FW_ID << 16) | value;
}

void sep_bl1_post(const struct sep_bl1_ctx *ctx, uint8_t msg_type, uint16_t value);

/** @brief "BL0S", at both ends of the state block SEP BL0 hands to BL1 */
#define SEP_BL1_BL0_STATE_MAGIC 0x53304c42U

/** BUN1 TOC slot holding SMC BL0P5. */
#define SEP_BL1_BUN1_SMC_BL0P5_TOC_INDEX 1U

/**
 * BUN1 TOC slot SMC BL0P5 reads the SERDES ICCM+DCCM image from, in place in
 * the staging window (drivers/pcie/tt_grendel_pcie.c).
 */
#define SEP_BL1_BUN1_SERDES_TOC_INDEX 2U

/**
 * @brief Find the manifest SEP BL0 left in SEP SRAM.
 *
 * BL0 places its state block so that it ends at @a state_end. The last two
 * words are the block size and a closing magic; the first two are an opening
 * magic and the SEP address of the manifest.
 *
 * @return 0 on success, -ENOENT if no valid state block is present
 */
int sep_bl1_bl0_manifest_addr(const uint8_t *state_end, uint32_t *manifest_addr);

/**
 * @brief Copy a bundle into @a dst as one manifest-then-payload image.
 *
 * SEP BL0 keeps the manifest in SEP SRAM and the payload wherever it copied
 * it, with payload_offset rewritten to suit. SMC BL0P5 instead expects both
 * in its staging window. The payload may already overlap @a dst; the
 * manifest must not.
 *
 * On success the copy at @a dst has payload_offset == manifest_length.
 */
int sep_bl1_stage_bundle(const struct fw_bundle_manifest *manifest, const uint8_t *payload,
			 uint8_t *dst, size_t dst_size, const struct sep_bl1_ctx *ctx);

/**
 * @brief Find the TOC of a bundle staged at @a buf.
 *
 * Only checks that a TOC sits at payload_offset and fits in @a size. Nothing
 * is authenticated.
 *
 * @return the TOC, or NULL if @a buf does not hold a bundle
 */
const struct fw_bundle_toc *sep_bl1_bundle_toc(const uint8_t *buf, size_t size);

/**
 * @brief Copy SMC BL0P5 from the bundle at @a buf to its SMC load address.
 *
 * BL0P5 is taken from @ref SEP_BL1_BUN1_SMC_BL0P5_TOC_INDEX.
 */
int sep_bl1_load_smc_bl0p5(const uint8_t *buf, size_t size, const struct sep_bl1_ctx *ctx,
			   const struct fw_bundle_toc_entry **out);

/**
 * Program SMC reset vector 0 and release core 0.
 */
int sep_bl1_start_smc(const struct fw_bundle_toc_entry *smc_bl0p5, const struct sep_bl1_ctx *ctx,
		      uint64_t reset_vector_addr, uint64_t reset_ctrl_addr);

/**
 * Unsecured BUN2 ACK: check a bundle is staged, then set VALIDATED so SMC
 * BL0P5 can proceed. Hash and signature checks are deferred.
 */
int sep_bl1_ack_bun2_unsecured(uint32_t *scratch, const uint8_t *buf, size_t size,
			       const struct sep_bl1_ctx *ctx);

#ifdef CONFIG_SOC_TT_KERAUNOS_SEP
void sep_keraunos_init_ctx(struct sep_bl1_ctx *ctx);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TENSTORRENT_SEP_BL1_H_ */
