/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TENSTORRENT_SEP_BL1_H_
#define TENSTORRENT_SEP_BL1_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <tenstorrent/fw_bundle.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Status word: [31:24] message type, [23:16] firmware id, [15:0] value.
 * SEP BL1 firmware id is 2.
 */
#define SEP_BL1_MSG_STATUS 0x01U
#define SEP_BL1_MSG_WARNING 0x08U
#define SEP_BL1_MSG_ERROR 0x0FU

#define SEP_BL1_FW_ID 0x02U

#define SEP_BL1_STATUS_BOOT_START 0x0010U
#define SEP_BL1_STATUS_PRIMARY_MODE 0x0021U
#define SEP_BL1_STATUS_BOOT_COMPLETE 0x0050U
#define SEP_BL1_STATUS_UNEXPECTED_EXIT 0x00FFU
#define SEP_BL1_ERROR_VALIDATE 0x0140U
#define SEP_BL1_ERROR_MISSING_IMAGE 0x0142U
#define SEP_BL1_ERROR_ENTRY_POINT 0x0143U
#define SEP_BL1_ERROR_HASH 0x0144U
#define SEP_BL1_ERROR_POLICY 0x0145U
#define SEP_BL1_ERROR_HW_UNAVAILABLE 0x0146U

/**
 * BUN2 validation handshake bits. Must stay identical to SMC BL0P5
 * (app/bl0p5/src/main.c): bit0 = ready for validation, bit1 = validated.
 */
#define SEP_BL1_BUNDLE_READY_FOR_VALIDATION_BIT BIT(0)
#define SEP_BL1_BUNDLE_VALIDATED_BIT BIT(1)

/**
 * Bundle staging handshake, in the host-boot-state scratch register. Must stay
 * identical to SMC BL0P5 (app/bl0p5/src/main.c), which blocks on BUNDLE_STAGED
 * before it ever raises the validation doorbell, and reports BUNDLE_CONSUMED
 * once it has taken SMC BL1 out of the staging area.
 */
#define SEP_BL1_HOST_BOOT_STATE_MASK 0xFU
#define SEP_BL1_HOST_BOOT_STATE_WAIT_FOR_BUNDLE 1U
#define SEP_BL1_HOST_BOOT_STATE_BUNDLE_STAGED 2U
#define SEP_BL1_HOST_BOOT_STATE_BUNDLE_CONSUMED 3U

#define SEP_BL1_STATUS_EXTRACT_MSG_TYPE(v) ((uint8_t)(((v) >> 24) & 0xFF))
#define SEP_BL1_STATUS_EXTRACT_FW_ID(v) ((uint8_t)(((v) >> 16) & 0xFF))
#define SEP_BL1_STATUS_EXTRACT_VALUE(v) ((uint16_t)((v) & 0xFFFF))

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
	/**
	 * @brief Validate a SEP image entry point against platform rules.
	 *
	 * @return 0 if the CPU may jump to @a entry_point
	 */
	int (*validate_sep_entry)(uint64_t load_addr, uint64_t entry_point, uint64_t length);
	/**
	 * @brief Transfer control to SEP mission firmware. Must not return.
	 * @return negative errno if the jump cannot be performed
	 */
	int (*enter_mission)(uint64_t entry_point);
	/** @brief Program HW security version; required if the manifest requests it */
	int (*update_security_version)(uint16_t version);
	/** @brief Lock demotion; required if the manifest requests BL1 demotion */
	int (*lock_demotion)(void);
};

struct sep_bl1_ctx {
	struct tt_fw_bundle_device_info device;
	struct tt_fw_bundle_auth_ops auth;
	struct sep_bl1_hw_ops hw;
};

static inline uint32_t sep_bl1_status_word(uint8_t msg_type, uint16_t value)
{
	return ((uint32_t)msg_type << 24) | ((uint32_t)SEP_BL1_FW_ID << 16) | value;
}

void sep_bl1_post(const struct sep_bl1_ctx *ctx, uint8_t msg_type, uint16_t value);

/**
 * @brief Authenticate a staged BUN2 and apply policy that SEP BL1 owns.
 *
 * Locates SMC BL1 and optional SEP mission images. Does not copy SMC
 * images (SMC BL0P5 does that) and does not jump.
 */
int sep_bl1_validate_staged_bundle(const uint8_t *buf, size_t size, const struct sep_bl1_ctx *ctx,
				   struct tt_fw_bundle_view *view,
				   const struct fw_bundle_toc_entry **smc_bl1,
				   const struct fw_bundle_toc_entry **sep_mis);

/**
 * @brief Service one BUN2 validation doorbell in @a scratch.
 *
 * If READY is clear, returns 0 without changing *scratch.
 * On success sets VALIDATED. On failure leaves VALIDATED clear and posts
 * an error status.
 */
int sep_bl1_service_validation_scratch(uint32_t *scratch, const uint8_t *buf, size_t size,
				       const struct sep_bl1_ctx *ctx,
				       struct tt_fw_bundle_view *view);

/**
 * @brief Handoff to SEP mission firmware from a validated view.
 *
 * @return -ENOENT if no SEP_MIS image, -ENOTSUP if enter_mission is NULL,
 *         -EIO if enter_mission returns (unexpected).
 */
int sep_bl1_enter_mission_firmware(const struct tt_fw_bundle_view *view,
				   const struct sep_bl1_ctx *ctx);

/**
 * Unsecured bring-up helpers. Hash/signature checks are deferred. Images
 * are taken from a buffer already in the staging window.
 */
int sep_bl1_copy_image(const struct tt_fw_bundle_view *view,
		       const struct fw_bundle_toc_entry *entry, const struct sep_bl1_ctx *ctx);

int sep_bl1_load_smc_bl0p5(const struct tt_fw_bundle_view *view, const struct sep_bl1_ctx *ctx,
			   const struct fw_bundle_toc_entry **out);

/**
 * Copy every TOC image whose load_addr lies in @a sram_base..+@a sram_size.
 * There is no SERDES image-type in tt_bundle_loader.h yet, so destination
 * match is the contract.
 */
int sep_bl1_place_serdes(const struct tt_fw_bundle_view *view, const struct sep_bl1_ctx *ctx,
			 uint64_t sram_base, uint64_t sram_size, int *placed);

/**
 * Program SMC reset vector 0 and release core 0.
 */
int sep_bl1_start_smc(const struct fw_bundle_toc_entry *smc_bl0p5, const struct sep_bl1_ctx *ctx,
		      uint64_t reset_vector_addr, uint64_t reset_ctrl_addr);

/**
 * Unsecured BUN2 ACK: parse only, then set VALIDATED so SMC BL0P5 can proceed.
 */
int sep_bl1_ack_bun2_unsecured(uint32_t *scratch, const uint8_t *buf, size_t size,
			       const struct sep_bl1_ctx *ctx, struct tt_fw_bundle_view *view);

#ifdef CONFIG_SOC_TT_KERAUNOS_SEP
void sep_keraunos_init_ctx(struct sep_bl1_ctx *ctx);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TENSTORRENT_SEP_BL1_H_ */
