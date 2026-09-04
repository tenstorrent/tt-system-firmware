/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TENSTORRENT_FW_BUNDLE_H_
#define TENSTORRENT_FW_BUNDLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/misc/tt_bundle_loader.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Current manifest/TOC major version this parser understands */
#define TT_FW_BUNDLE_FMT_MAJOR 1U

/** @brief Maximum v1.x manifest length accepted by SEP BL0 (bytes) */
#define TT_FW_BUNDLE_MANIFEST_MAX_LEN 2048U

/** @brief Packed size of a v1.0 manifest */
#define TT_FW_BUNDLE_MANIFEST_V1_SIZE 1184U

/** @brief Packed size of the TOC header (excluding entries) */
#define TT_FW_BUNDLE_TOC_HDR_SIZE 32U

/** @brief Packed size of one TOC entry */
#define TT_FW_BUNDLE_TOC_ENTRY_SIZE 216U

/** @brief Image start offsets must be a multiple of this */
#define TT_FW_BUNDLE_IMAGE_ALIGN 8U

/**
 * @brief Device-side inputs for policy checks.
 *
 * Hardware (OTP/fuses) is not read here. The caller supplies values obtained
 * from a platform HAL. NULL identity pointers skip the corresponding selector
 * match only when that selector bit is clear; if a selector bit is set and
 * the pointer is NULL the check fails.
 */
struct tt_fw_bundle_device_info {
	/** @brief True when the device is in secure boot */
	bool secure_boot;
	/** @brief Device lifecycle; compared bitwise against the manifest mask */
	union fw_bundle_lc_state lifecycle;
	/** @brief Hardware anti-rollback version */
	uint16_t hw_security_version;
	/** @brief Bitmask of allowed @ref fw_bundle_public_key_sel.key_sel values */
	uint8_t allowed_key_sel_mask;
	/** @brief Bit i set means ROM key index i is revoked */
	uint16_t revoked_key_index_mask;
	/** @brief 32-byte CHIPLET_ID from OTP, or NULL */
	const uint8_t *chiplet_id;
	/** @brief 32-byte PACKAGE_ID from OTP, or NULL */
	const uint8_t *package_id;
};

/**
 * @brief Optional crypto hooks.
 *
 * Hashing of TOC/images is performed with @ref tt_fw_bundle_sha256.
 * Signature verification is not implemented in this layer: RSA-3072/ECC
 * verification belongs with the SEP key/fuse HAL. When @a secure_boot is
 * true, @ref verify_manifest must be non-NULL or validation fails with
 * -ENOTSUP rather than succeeding.
 */
struct tt_fw_bundle_auth_ops {
	/**
	 * @brief Verify the manifest signature over the signed prefix.
	 *
	 * @param manifest Manifest to authenticate
	 * @return 0 on success, negative errno on failure
	 */
	int (*verify_manifest)(const struct fw_bundle_manifest *manifest);
};

/** @brief Parsed view of a staged bundle; pointers alias the caller's buffer */
struct tt_fw_bundle_view {
	const struct fw_bundle_manifest *manifest;
	const struct fw_bundle_toc *toc;
	const uint8_t *payload;
	size_t payload_len;
	const uint8_t *base;
	size_t size;
};

/**
 * @brief SHA-256 over @a data.
 *
 * @param data Input bytes
 * @param len Length in bytes
 * @param out 32-byte digest
 * @return 0 on success, -EINVAL if arguments are invalid
 */
int tt_fw_bundle_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/**
 * @brief Locate the TOC for a manifest already known to be in-bounds.
 *
 * Does not authenticate. @a view is filled on success.
 */
int tt_fw_bundle_parse(const uint8_t *buf, size_t size, struct tt_fw_bundle_view *view);

/**
 * @brief Structural, policy, and hash validation of a staged bundle.
 *
 * Does not perform image copies or jumps. Does not program fuses.
 */
int tt_fw_bundle_validate(const uint8_t *buf, size_t size,
			  const struct tt_fw_bundle_device_info *dev,
			  const struct tt_fw_bundle_auth_ops *auth,
			  struct tt_fw_bundle_view *view);

/**
 * @brief Return the first TOC entry whose @a type matches, or NULL.
 */
const struct fw_bundle_toc_entry *tt_fw_bundle_find_image(const struct tt_fw_bundle_view *view,
							  uint64_t type);

/**
 * @brief Pointer to image bytes inside the caller's buffer.
 */
const uint8_t *tt_fw_bundle_image_data(const struct tt_fw_bundle_view *view,
				       const struct fw_bundle_toc_entry *entry);

/**
 * @brief Return the TOC entry_point unchanged.
 *
 * The field is image-type-specific and may be an absolute CPU address or a
 * value in another address space (for example IRAM-relative). This helper
 * never relocates it.
 */
uint64_t tt_fw_bundle_entry_point(const struct fw_bundle_toc_entry *entry);

#ifdef __cplusplus
}
#endif

#endif /* TENSTORRENT_FW_BUNDLE_H_ */
