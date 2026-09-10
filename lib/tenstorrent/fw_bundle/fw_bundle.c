/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <tenstorrent/fw_bundle.h>
#include <zephyr/sys/util.h>

BUILD_ASSERT(sizeof(struct fw_bundle_manifest) == TT_FW_BUNDLE_MANIFEST_V1_SIZE,
	     "fw_bundle_manifest must match the v1.0 1184-byte layout");
BUILD_ASSERT(sizeof(struct fw_bundle_toc) == TT_FW_BUNDLE_TOC_HDR_SIZE,
	     "fw_bundle_toc header must be 32 bytes");
BUILD_ASSERT(sizeof(struct fw_bundle_toc_entry) == TT_FW_BUNDLE_TOC_ENTRY_SIZE,
	     "fw_bundle_toc_entry must be 224 bytes");

static bool u64_add_ok(uint64_t a, uint64_t b, uint64_t *sum)
{
	if (a > UINT64_MAX - b) {
		return false;
	}
	*sum = a + b;
	return true;
}

static int payload_bounds(const uint8_t *buf, size_t size, const struct fw_bundle_manifest *m,
			  const uint8_t **payload, size_t *payload_len)
{
	uint64_t abs_off;
	uint64_t end;

	/*
	 * This parser views @buf as starting at the manifest. Negative
	 * payload_offset (payload placed before the manifest) is a valid
	 * flash packing trick for BL0, but it is not representable in a
	 * buffer that begins at the manifest. Reject it here rather than
	 * guessing an IRAM/SPI window.
	 */
	if (m->payload_offset < 0) {
		return -ENOTSUP;
	}

	abs_off = (uint64_t)m->payload_offset;
	if (abs_off > size) {
		return -EINVAL;
	}
	if (!u64_add_ok(abs_off, m->payload_length, &end) || end > size) {
		return -EINVAL;
	}

	*payload = buf + abs_off;
	*payload_len = (size_t)m->payload_length;
	return 0;
}

int tt_fw_bundle_parse(const uint8_t *buf, size_t size, struct tt_fw_bundle_view *view)
{
	const struct fw_bundle_manifest *m;
	const uint8_t *payload;
	size_t payload_len;
	const struct fw_bundle_toc *toc;
	uint64_t toc_bytes;
	int rc;

	if (buf == NULL || view == NULL) {
		return -EINVAL;
	}
	if (size < TT_FW_BUNDLE_MANIFEST_V1_SIZE) {
		return -EINVAL;
	}

	m = (const struct fw_bundle_manifest *)buf;
	if (m->manifest_identifier != MANIFEST_ID_BL1 && m->manifest_identifier != MANIFEST_ID_MIS) {
		return -EINVAL;
	}
	if (m->manifest_version_major != TT_FW_BUNDLE_FMT_MAJOR) {
		return -EINVAL;
	}
	if (m->manifest_length < TT_FW_BUNDLE_MANIFEST_V1_SIZE ||
	    m->manifest_length > TT_FW_BUNDLE_MANIFEST_MAX_LEN || (m->manifest_length % 8U) != 0U) {
		return -EINVAL;
	}
	if (m->manifest_length > size) {
		return -EINVAL;
	}

	rc = payload_bounds(buf, size, m, &payload, &payload_len);
	if (rc != 0) {
		return rc;
	}
	if (payload_len < TT_FW_BUNDLE_TOC_HDR_SIZE) {
		return -EINVAL;
	}

	toc = (const struct fw_bundle_toc *)payload;
	if (toc->toc_identifier != FW_BUNDLE_TOC_ID) {
		return -EINVAL;
	}
	if (toc->toc_version_major != TT_FW_BUNDLE_FMT_MAJOR) {
		return -EINVAL;
	}
	if (toc->image_count > (SIZE_MAX - TT_FW_BUNDLE_TOC_HDR_SIZE) / TT_FW_BUNDLE_TOC_ENTRY_SIZE) {
		return -EINVAL;
	}
	toc_bytes = TT_FW_BUNDLE_TOC_HDR_SIZE + toc->image_count * TT_FW_BUNDLE_TOC_ENTRY_SIZE;
	if (toc_bytes > payload_len) {
		return -EINVAL;
	}
	if (!m->authenticated_flags.f.encrypted_payload && toc->payload_length != m->payload_length) {
		return -EINVAL;
	}

	memset(view, 0, sizeof(*view));
	view->manifest = m;
	view->toc = toc;
	view->payload = payload;
	view->payload_len = payload_len;
	view->base = buf;
	view->size = size;
	return 0;
}

static int check_selectors(const struct fw_bundle_manifest *m,
			   const struct tt_fw_bundle_device_info *dev)
{
	unsigned int i;

	for (i = 0; i < 8U; i++) {
		if ((m->selector_bits & BIT64(i)) == 0U) {
			continue;
		}
		if (dev->chiplet_id == NULL ||
		    memcmp(&m->chiplet_id[i * 4U], &dev->chiplet_id[i * 4U], 4U) != 0) {
			return -EPERM;
		}
	}
	for (i = 0; i < 8U; i++) {
		if ((m->selector_bits & BIT64(8U + i)) == 0U) {
			continue;
		}
		if (dev->package_id == NULL ||
		    memcmp(&m->package_id[i * 4U], &dev->package_id[i * 4U], 4U) != 0) {
			return -EPERM;
		}
	}
	return 0;
}

static int check_policy(const struct fw_bundle_manifest *m,
			const struct tt_fw_bundle_device_info *dev)
{
	int rc;

	if (!dev->secure_boot) {
		return 0;
	}

	if ((m->lifecycle_states.u32_all & dev->lifecycle.u32_all) == 0U) {
		return -EPERM;
	}
	if (m->security_version < dev->hw_security_version) {
		return -EPERM;
	}
	if ((BIT(m->public_key_sel.f.key_sel) & dev->allowed_key_sel_mask) == 0U) {
		return -EPERM;
	}
	if (m->public_key_sel.f.key_index >= 16U ||
	    (dev->revoked_key_index_mask & BIT(m->public_key_sel.f.key_index)) != 0U) {
		return -EPERM;
	}

	rc = check_selectors(m, dev);
	if (rc != 0) {
		return rc;
	}
	return 0;
}

static int check_hashes(const struct tt_fw_bundle_view *view)
{
	const struct fw_bundle_manifest *m = view->manifest;
	uint8_t digest[32];
	uint64_t hashed_len = m->payload_hashed_length;
	uint64_t i;
	int rc;

	if (m->authenticated_flags.f.encrypted_payload) {
		if (hashed_len != m->payload_length) {
			return -EINVAL;
		}
	} else {
		uint64_t toc_bytes =
			TT_FW_BUNDLE_TOC_HDR_SIZE + view->toc->image_count * TT_FW_BUNDLE_TOC_ENTRY_SIZE;

		if (hashed_len != toc_bytes) {
			return -EINVAL;
		}
	}
	if (hashed_len > view->payload_len) {
		return -EINVAL;
	}

	rc = tt_fw_bundle_sha256(view->payload, (size_t)hashed_len, digest);
	if (rc != 0) {
		return rc;
	}
	if (memcmp(digest, m->payload_hash, sizeof(digest)) != 0) {
		return -EBADMSG;
	}

	/* Signed-region hash is required for secure and non-secure manifests. */
	rc = tt_fw_bundle_sha256((const uint8_t *)m, offsetof(struct fw_bundle_manifest, signature),
				 digest);
	if (rc != 0) {
		return rc;
	}
	if (memcmp(digest, m->manifest_hash, sizeof(digest)) != 0) {
		return -EBADMSG;
	}

	for (i = 0; i < view->toc->image_count; i++) {
		const struct fw_bundle_toc_entry *e = &view->toc->entries[i];
		const uint8_t *img;

		if ((e->offset % TT_FW_BUNDLE_IMAGE_ALIGN) != 0U) {
			return -EINVAL;
		}
		if (e->offset > view->payload_len || e->length > view->payload_len - e->offset) {
			return -EINVAL;
		}

		img = tt_fw_bundle_image_data(view, e);
		rc = tt_fw_bundle_sha256(img, (size_t)e->length, digest);
		if (rc != 0) {
			return rc;
		}
		if (memcmp(digest, e->hash, sizeof(digest)) != 0) {
			return -EBADMSG;
		}
	}

	return 0;
}

int tt_fw_bundle_validate(const uint8_t *buf, size_t size,
			  const struct tt_fw_bundle_device_info *dev,
			  const struct tt_fw_bundle_auth_ops *auth, struct tt_fw_bundle_view *view)
{
	struct tt_fw_bundle_view local;
	int rc;

	if (dev == NULL || view == NULL) {
		return -EINVAL;
	}

	rc = tt_fw_bundle_parse(buf, size, &local);
	if (rc != 0) {
		return rc;
	}

	rc = check_policy(local.manifest, dev);
	if (rc != 0) {
		return rc;
	}

	if (dev->secure_boot) {
		if (auth == NULL || auth->verify_manifest == NULL) {
			return -ENOTSUP;
		}
		rc = auth->verify_manifest(local.manifest);
		if (rc != 0) {
			return rc;
		}
	}

	rc = check_hashes(&local);
	if (rc != 0) {
		return rc;
	}

	*view = local;
	return 0;
}

const struct fw_bundle_toc_entry *tt_fw_bundle_find_image(const struct tt_fw_bundle_view *view,
							  uint64_t type)
{
	uint64_t i;

	if (view == NULL || view->toc == NULL) {
		return NULL;
	}
	for (i = 0; i < view->toc->image_count; i++) {
		if (view->toc->entries[i].type == type) {
			return &view->toc->entries[i];
		}
	}
	return NULL;
}

const uint8_t *tt_fw_bundle_image_data(const struct tt_fw_bundle_view *view,
				       const struct fw_bundle_toc_entry *entry)
{
	if (view == NULL || view->payload == NULL || entry == NULL) {
		return NULL;
	}
	return view->payload + entry->offset;
}

uint64_t tt_fw_bundle_entry_point(const struct fw_bundle_toc_entry *entry)
{
	if (entry == NULL) {
		return 0;
	}
	return entry->entry_point;
}
