/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TT_BUNDLE_LOADER_H
#define TT_BUNDLE_LOADER_H

#include <errno.h>
#include <stdint.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Indicates a word in the manifest is unused*/
#define FW_BUNDLE_MANIFEST_UNUSED_WORD (0xA5A5A5A5U)

/** @brief "TBL1" - BL1 manifest */
#define MANIFEST_ID_BL1 (0x314c4254U)

/** @brief "TBL2" - MIS manifest */
#define MANIFEST_ID_MIS (0x324c4254U)

/** @brief "PTOC" - FW_BUNDLE_TOC_ID*/
#define FW_BUNDLE_TOC_ID (0x434f5450U)

/** @brief Base address of the BUN2 staging area in SMC local address space */
#define TT_BUN2_STAGING_AREA_ADDR 0xc0066400UL

/** @brief Base address of the BUN3 staging area in SMC local address space */
#define TT_BUN3_STAGING_AREA_ADDR 0xc0066400UL

/** @brief AES-128-CBC */
#define FW_BUNDLE_ENCRYPTION_TYPE_AES (0x01)

/** @brief RSA-3072, RSASSA-PKCS1-v1_5, SHA-256 */
#define FW_BUNDLE_SIGNATURE_TYPE_RSA (0x01)

/** @brief ECC-P256 */
#define FW_BUNDLE_SIGNATURE_TYPE_ECC_P256 (0x02)

/** @brief Use key at key index from ROM */
#define FW_BUNDLE_FUSE_KEY_SEL_ROM (0x0)

/** @brief Use PUBK_HASH0 from fuses */
#define FW_BUNDLE_FUSE_KEY_SEL_PUBK_HASH0 (0x1)

/** @brief Use PUBK_HASH1 from fuses */
#define FW_BUNDLE_FUSE_KEY_SEL_PUBK_HASH1 (0x2)

/** @brief Use SoP_PUBK from fuses */
#define FW_BUNDLE_FUSE_KEY_SEL_SOP_PUBK (0x4)

/** @brief Use SYS_PUBK from fuses */
#define FW_BUNDLE_FUSE_KEY_SEL_SYS_PUBK (0x5)

/** @brief SEPBL1 image type */
#define FW_BUNDLE_IMG_TYPE_SEP_BL1 (0x0000314c42504553ULL)

/** @brief SEP Mission image type */
#define FW_BUNDLE_IMG_TYPE_SEP_MIS (0x0000324c42504553ULL)

/** @brief SMCBL0.5 image type*/
#define FW_BUNDLE_IMG_TYPE_SMC_BL0P5 (0x3550304c42434d53ULL)

/** @brief SMC BL1 image type */
#define FW_BUNDLE_IMG_TYPE_SMC_BL1 (0x0000314c42434d53ULL)

/** @brief SMC mission image type */
#define FW_BUNDLE_IMG_TYPE_SMC_MIS (0x0000324c42434d53ULL)

/** @brief Lifecycle state selection
 * This field controls whether or not this image is loaded on a device based on the
 * lifecycle state of that device. This is only enforced when secure boot is
 * enabled.
 */
union fw_bundle_lc_state {
	/** @brief The field as u32*/
	uint32_t u32_all;
	struct {
		/** @brief lifecycle state TEST_DEV allowed*/
		uint32_t test_dev: 1;

		/** @brief lifecycle state PROD allowed*/
		uint32_t prod: 1;

		/** @brief lifecycle state PROD_END allowed*/
		uint32_t prod_end: 1;

		/** @brief lifecycle state RMA_SIP allowed*/
		uint32_t rma_sip: 1;

		/** @brief lifecycle state RMA_CHIPLET allowed*/
		uint32_t rma_chiplet: 1;
	} f;
};

/** @brief Authenticated flags field
 * Some sub-fields in flags are subject to bits in the
 * selector_bits field.
 */
union fw_bundle_auth_flags {
	/** @brief The field as a u32*/
	uint32_t u32_all;

	struct {
		/** @brief BL1 demotion request, only valid when selector_bits[17] is 1 */
		uint32_t bl1_demotion: 1;

		/** @brief Payload is encrypted */
		uint32_t encrypted_payload: 1;

		/** @brief Update the hardware security version to match @ref security_version */
		uint32_t security_version_update: 1;

		/** @brief Reserved, must be 0 */
		uint32_t reserved: 29;
	} f;
};

union fw_bundle_public_key_sel {
	/** @brief The field as a u16*/
	uint16_t u16_all;

	struct {
		/** @brief Key index in ROM */
		uint16_t key_index: 4;

		/** @brief ROM / Fuse Key Selection
		 * Allowed values:
		 *	@ref FW_BUNDLE_FUSE_KEY_SEL_PUBK_HASH0
		 *	@ref FW_BUNDLE_FUSE_KEY_SEL_PUBK_HASH1
		 *	@ref FW_BUNDLE_FUSE_KEY_SEL_SOP_PUBK
		 *	@ref FW_BUNDLE_FUSE_KEY_SEL_SYS_PUBK
		 */
		uint16_t key_sel: 3;

		/** @brief Reserved - must be 0 */
		uint16_t reserved: 9;
	} f;
};

union fw_bundle_sem_ver {
	/** @brief The field as a u64*/
	uint64_t u64_all;

	struct {
		/** @brief Patch version */
		uint64_t patch_version: 24;

		/** @brief Minor version */
		uint64_t minor_version: 24;

		/** @brief Major version */
		uint64_t major_version: 16;
	} f;
};

union fw_bundle_unauthed_flags {
	/** @brief The field as a u32*/
	uint32_t u32_all;

	struct {
		/** @brief MIS demotion
		 * When @ref secure_boot == 1, indicates that the MIS manifest
		 * will demand demotion. This causes the SEP BL0 to not lock the demotion
		 * register to allow a later firmware to perform the lock. The demotion key
		 * derivation is used when this is selected so that production secrets are not
		 * revealed.
		 */
		uint32_t mis_demotion: 1;

		/** @brief Reserved, must be 0 */
		uint32_t reserved: 23;

		/** @brief Reserved for implementation defined use */
		uint32_t impl_reserved: 8;
	} f;
};

struct fw_bundle_manifest {
	/** @brief The manifest identifier
	 * The manifest_identifier is used to identify which boot stage payload this manifest
	 * describes. These values should be 4 character ASCII strings so that hex dumps of a
	 * manifest can be identified visually. Defined values are below. All other values are
	 * reserved. Tenstorrent may introduce new values to this field in the future. Hex values
	 * are in little-endian format.
	 * @ref MANIFEST_ID_BL1
	 * @ref MANIFEST_ID_MIS
	 */
	uint32_t manifest_identifier;

	/** @brief The major version of the manifest
	 * Major version of the manifest format that the manifest conforms to. Current major
	 * version is 1. Changes to the major version indicate a change that is not backwards
	 * compatible. If the major version number in a manifest is greater than the major
	 * version for which the software was designed to use the software will reject the
	 * manifest. Old software can correctly use a format that only differs by having a larger
	 * minor version than it was designed to work with.
	 */
	uint16_t manifest_version_major;

	/** @brief The minor version of the manifest
	 * Minor version of the manifest format. Current minor version is 0.
	 */
	uint16_t manifest_version_minor;

	/** @brief The Length of the manifest in bytes
	 * Manifest version 1.0 is 1184 bytes. The manifest will
	 * always be a multiple of 8 bytes in length. This ensures proper alignment of fields when
	 * manifests are packed together in flash or RAM. The SEP BL0 ROM supports a v1.x
	 * manifest ⇐ 2048 bytes in size.
	 */
	uint32_t manifest_length;

	/** @brief Reserved. Must be 0x0 */
	uint32_t reserved;

	/** @brief selector bits */
	uint64_t selector_bits;

	/** @brief Chiplet identifier
	 * Chiplet identifier value which is compared against the CHIPLET_ID value stored in
	 * the SEP OTP. Mapped to bits [7:0] of selector_bits. Unselected words must be set to
	 * @ref FW_BUNDLE_MANIFEST_UNUSED_WORD. See detailed description below.
	 */
	uint8_t chiplet_id[32];

	/** @brief Package identifier
	 * Package identifier value which is compared against the PACKAGE_ID value stored in
	 * the SEP OTP. Mapped to bits [15:8] of selector_bits. Unselected words must be set to
	 * @ref FW_BUNDLE_MANIFEST_UNUSED_WORD. See detailed description below.
	 */
	uint8_t package_id[32];

	/** @brief lifecycle state control */
	union fw_bundle_lc_state lifecycle_states;

	/** @brief Authenticated flags */
	union fw_bundle_auth_flags authenticated_flags;

	/** @brief Encryption initial vector
	 * Only valid when encrypted_payload = 1
	 */
	uint8_t encryption_iv[32];

	/** @brief Key Derivation Function's input to derive the key used for encryption
	 * Only valid when encrypted_payload =1
	 */
	uint8_t encryption_kdf_input[32];

	/** @brief Reserved - must be 0 */
	uint16_t reserved_0;

	/** @brief Security version of the manifest
	 * Used for anti-rollback protection. Must be a monotonically increasing integer. Only valid
	 * when boot is secure. When flags.security_version_update == 1, the hardware security
	 * version is updated to match the version in this field by SEP BL1 firmware. Maximum
	 * security version is device dependent.
	 */
	uint16_t security_version;

	/** @brief Encryption algorithm
	 * Used when authenticated_flags.encrypted_payload == 1.
	 * Allowed values:
	 *	@ref FW_BUNDLE_ENCRYPTION_TYPE_AES
	 *	0xE0-0xEF: chiplet implementation defined
	 *	0xF0-0xFF: Package implementation defined
	 *	All other values reserved
	 */
	uint8_t encryption_type;

	/** @brief Signature algorithm, including padding/hash used for signature of the manifest.
	 * Ignored when boot is not secure.
	 * Allowed values:
	 *	@ref FW_BUNDLE_SIGNATURE_TYPE_RSA
	 *	@ref FW_BUNDLE_SIGNATURE_TYPE_ECC_P256
	 *	0xE0-0xEF: chiplet implementation defined
	 *	0xF0-0xFF: Package implementation defined
	 *	All other values: reserved
	 */
	uint8_t signature_type;

	/** @brief Public key selector requested by software.
	 * This field contains the index of the public
	 * key to be used for authentication. The valid range is determined by the number of keys
	 * in the hardware. The selected key needs to be an unrevoked key in the hardware. Boot
	 * fails with an error if this value is out of range or selects a revoked key.
	 */
	union fw_bundle_public_key_sel public_key_sel;

	/** @brief Public key used for authentication.
	 * This field is sized to support the maximum size key which is RSA 3072. Key types that do
	 * not use all of the space occupy the beginning of this field. They unused bits in this
	 * field are 0x0.
	 */
	uint8_t public_key[384];

	/** @brief SHA-256 hash digest of the portion of the payload covered by this hash.
	 * See @ref payload_hashed_length for more information.
	 */
	uint8_t payload_hash[32];

	/** @brief Number of bytes of the payload that are covered by @ref payload_hash.
	 * For encrypted payloads this length must be equal to the payload length, as the entire
	 * encrypted payload must be authenticated before it is decrypted. For cleartext payloads
	 * this is the length of the TOC (Table of Contents) portion of the payload. The TOC
	 * contains hashes for all images in the payload. This transitive authentication allows
	 * for more flexible processing of images in cleartext payloads.
	 */
	uint64_t payload_hashed_length;

	/** @brief Unix timestamp (64 bit) of the creation time of the manifest.
	 * When secure_boot == 1 this is the timestamp of when the signature was generated. This
	 * timestamp is not used for security checks.
	 */
	int64_t timestamp;

	/** @brief Length of payload in bytes.
	 * The payload consists of all data between payload_offset and payload_offset +
	 * payload_length. When the payload is encrypted this includes any padding added when
	 * encryption is performed. For cleartext payloads this length must match the
	 * payload_length in the payload TOC.
	 */
	uint64_t payload_length;

	/** @brief Version for the payload described by this manifest. */
	union fw_bundle_sem_ver manifest_content_version;

	/** @brief ASCII NULL terminated string describing the manifest and payload.
	 * This is provided to make identification binary manifests more precise and human readable.
	 * This is expected to contain version control details, release status and other useful
	 * information. This field is not used by firmware. The final byte of this field
	 * (ie manifest_description[127]) must be 0 and this will be set to 0 by software that uses
	 * this field to ensure NULL termination.
	 */
	uint8_t manifest_description[128];

	/** @brief The signature over all preceding fields in the manifest.
	 * The type of signature contained in this field is indicated by the @ref signature_type
	 * field. This field is sized to support the maximum size used by any of the supported
	 * signature types which is RSA-3072. When secure_boot == 0, this
	 * field is unused and ignored.
	 */
	uint8_t signature[384];

	/** @brief SHA-256 hash over the signed region of the manifest.
	 * This hash is required for both secure and non-secure manifests.
	 */
	uint8_t manifest_hash[32];

	/** @brief Offset in bytes from the start of the manifest to the start of the payload.
	 * This field may be updated when the manifest and payload are written to flash, so this
	 * cannot be part of the signed region of the manifest. A negative value indicates that the
	 * payload is located before the manifest.
	 */
	int64_t payload_offset;

	/** @brief Unauthenticated flags field */
	union fw_bundle_unauthed_flags flags;

	/** @brief Reserved, must be 0 */
	uint8_t reserved_1[12];
} __packed;

struct fw_bundle_toc_entry {
	/** @brief Defines type of image, as well as the destination component */
	uint64_t type;

	/** @brief Offset from the start of the payload to the start of this image.
	 * Must be a multiple of 8.
	 */
	uint64_t offset;

	/** @brief Length in bytes of this image
	 * This image occupies the space in the payload starting at offset bytes from the start of
	 * the payload TOC and continuing for length bytes. There can be 'unused' bytes in the
	 * payload between then end of one image and the beginning of another.
	 */
	uint64_t length;

	/** @brief version for the image described by this TOC entry. */
	union fw_bundle_sem_ver version;

	/** @brief Load address for this image.
	 * Load address for this image. Interpreted on a per-image type basis. May not apply to all
	 * image types.
	 */
	uint64_t load_addr;

	/** @brief Entry point for this image.
	 * Interpreted on a per-image type basis, may be in different address space than load_addr.
	 * May not apply to all images.
	 */
	uint64_t entry_point;

	/** @brief Reserved - must be 0 */
	uint64_t reserved;

	/** @brief SHA-256 hash over this image */
	uint8_t hash[32];

	/** @brief ASCII NULL terminated string describing the manifest and payload.
	 * This is provided to make identifying binary manifests more precise and human readable.
	 * This is expected to contain version control details, release status and other useful
	 * information. This field is not used by firmware. The final byte of this field (ie
	 * manifest_description[127]) must be 0 and this will be set to 0 by software that uses this
	 * field to ensure NULL termination.
	 */
	uint8_t description[128];
} __packed;

/** @brief The table of contents present in the FW bundle payload*/
struct fw_bundle_toc {
	/** @brief The toc identifier string. Must be @ref FW_BUNDLE_TOC_ID */
	uint32_t toc_identifier;

	/** @brief Major version of the TOC format.
	 * Current major version is 1. Changes to the major version indicate a change that
	 * is not backwards compatible. If the major version number in a manifest is greater
	 * than the major version for which the software was designed to use the software
	 * will reject the manifest. Old software can correctly use a format that only
	 * differs by having a larger minor version than it was designed to work with.
	 */
	uint16_t toc_version_major;

	/** @brief Minor version of the TOC format.
	 * Current minor version is 0.
	 */
	uint16_t toc_version_minor;

	/** @brief Length of the cleartext payload in bytes.
	 * This size includes the TOC, all images, and any (optional) space. This
	 * payload_length in the TOC may only differ from the manifest payload_length when
	 * the TOC payload_length excludes padding due to encryption of the payload. The
	 * payload_length may extend past the end of the images, as this is useful to allow
	 * in-place updates of images in flash; room for the entire payload_length must be
	 * allocated in flash. There may also be unused space in between images for various
	 * reasons including aligning the start address of images or allowing for in-place
	 * image updates in flash. Unused space in the payload is ignored and may have
	 * arbitrary values as flash blocks in this area may not be erased or written.
	 */
	uint64_t payload_length;

	/** @brief Number of images in the payload and TOC.
	 * The length of the TOC is determined by this.
	 */
	uint64_t image_count;

	/** @brief Reserved - must be 0 */
	uint64_t reserved;

	/** @brief The TOC entries, @ref image_count of them, immediately following this
	 * header
	 */
	struct fw_bundle_toc_entry entries[];
} __packed;

#ifdef __cplusplus
}
#endif

#endif /*TT_BUNDLE_LOADER_H*/
