/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <tenstorrent/fw_bundle.h>
#include <tenstorrent/sep_bl1.h>

#define MAX_BUNDLE 2048
#define IMG_LEN    8

static uint8_t g_bundle[MAX_BUNDLE];
static size_t g_bundle_len;
static uint32_t g_last_status;
static uint16_t g_secver;
static int g_demotion_locks;
static int g_mission_entered;
static uint64_t g_mission_entry;
static bool g_entry_ok = true;
static int g_sig_rc;

static void post_status(uint32_t word)
{
	g_last_status = word;
}

static int validate_entry(uint64_t load_addr, uint64_t entry_point, uint64_t length)
{
	ARG_UNUSED(load_addr);
	ARG_UNUSED(length);
	if (!g_entry_ok || entry_point == 0U) {
		return -EINVAL;
	}
	return 0;
}

static int enter_mission(uint64_t entry_point)
{
	g_mission_entered++;
	g_mission_entry = entry_point;
	return 0;
}

static int update_secver(uint16_t version)
{
	g_secver = version;
	return 0;
}

static int lock_demotion(void)
{
	g_demotion_locks++;
	return 0;
}

static int verify_ok(const struct fw_bundle_manifest *manifest)
{
	ARG_UNUSED(manifest);
	return g_sig_rc;
}

static struct tt_fw_bundle_device_info dev_ns(void)
{
	struct tt_fw_bundle_device_info d = {0};

	d.secure_boot = false;
	d.lifecycle.f.prod = 1;
	d.hw_security_version = 0;
	d.allowed_key_sel_mask = 0xFF;
	return d;
}

static struct sep_bl1_ctx make_ctx(bool secure)
{
	struct sep_bl1_ctx ctx = {0};

	ctx.device = dev_ns();
	ctx.device.secure_boot = secure;
	ctx.auth.verify_manifest = verify_ok;
	ctx.hw.post_status = post_status;
	ctx.hw.validate_sep_entry = validate_entry;
	ctx.hw.enter_mission = enter_mission;
	ctx.hw.update_security_version = update_secver;
	ctx.hw.lock_demotion = lock_demotion;
	return ctx;
}

static void fill_hashes(struct fw_bundle_manifest *m, struct fw_bundle_toc *toc, uint8_t *payload,
			size_t payload_len)
{
	uint64_t toc_bytes = TT_FW_BUNDLE_TOC_HDR_SIZE + toc->image_count * TT_FW_BUNDLE_TOC_ENTRY_SIZE;
	uint8_t digest[32];
	uint64_t i;

	/* Order matters. The per-image hashes live in the TOC, which is the region
	 * payload_hash covers, and payload_hash in turn sits inside the manifest
	 * prefix that manifest_hash covers. Hash innermost first.
	 */
	for (i = 0; i < toc->image_count; i++) {
		const uint8_t *img = payload + toc->entries[i].offset;

		zassert_ok(tt_fw_bundle_sha256(img, (size_t)toc->entries[i].length, digest));
		memcpy(toc->entries[i].hash, digest, sizeof(digest));
	}

	m->payload_hashed_length = toc_bytes;
	zassert_ok(tt_fw_bundle_sha256(payload, (size_t)toc_bytes, m->payload_hash));
	zassert_ok(tt_fw_bundle_sha256((const uint8_t *)m, offsetof(struct fw_bundle_manifest, signature),
				       m->manifest_hash));

	ARG_UNUSED(payload_len);
}

static size_t build_bundle(uint64_t type0, uint64_t load0, uint64_t entry0, uint64_t type1,
			   uint64_t load1, uint64_t entry1, bool include_mis)
{
	struct fw_bundle_manifest *m = (struct fw_bundle_manifest *)g_bundle;
	uint8_t *payload;
	struct fw_bundle_toc *toc;
	uint64_t nimg = include_mis ? 2U : 1U;
	uint64_t toc_bytes = TT_FW_BUNDLE_TOC_HDR_SIZE + nimg * TT_FW_BUNDLE_TOC_ENTRY_SIZE;
	uint64_t img0_off = toc_bytes;
	uint64_t img1_off = img0_off + IMG_LEN;
	uint64_t payload_len = include_mis ? (img1_off + IMG_LEN) : (img0_off + IMG_LEN);

	memset(g_bundle, 0, sizeof(g_bundle));
	m->manifest_identifier = MANIFEST_ID_BL1;
	m->manifest_version_major = 1;
	m->manifest_version_minor = 0;
	m->manifest_length = TT_FW_BUNDLE_MANIFEST_V1_SIZE;
	m->payload_offset = (int64_t)TT_FW_BUNDLE_MANIFEST_V1_SIZE;
	m->payload_length = payload_len;
	m->lifecycle_states.f.prod = 1;
	m->public_key_sel.f.key_sel = FW_BUNDLE_FUSE_KEY_SEL_PUBK_HASH0;
	m->security_version = 1;

	payload = g_bundle + TT_FW_BUNDLE_MANIFEST_V1_SIZE;
	toc = (struct fw_bundle_toc *)payload;
	toc->toc_identifier = FW_BUNDLE_TOC_ID;
	toc->toc_version_major = 1;
	toc->image_count = nimg;
	toc->payload_length = payload_len;

	toc->entries[0].type = type0;
	toc->entries[0].offset = img0_off;
	toc->entries[0].length = IMG_LEN;
	toc->entries[0].load_addr = load0;
	toc->entries[0].entry_point = entry0;
	memset(payload + img0_off, 0xA1, IMG_LEN);

	if (include_mis) {
		toc->entries[1].type = type1;
		toc->entries[1].offset = img1_off;
		toc->entries[1].length = IMG_LEN;
		toc->entries[1].load_addr = load1;
		toc->entries[1].entry_point = entry1;
		memset(payload + img1_off, 0xB2, IMG_LEN);
	}

	fill_hashes(m, toc, payload, (size_t)payload_len);
	g_bundle_len = TT_FW_BUNDLE_MANIFEST_V1_SIZE + (size_t)payload_len;
	return g_bundle_len;
}

static void *suite_setup(void)
{
	g_sig_rc = 0;
	g_entry_ok = true;
	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	g_last_status = 0;
	g_secver = 0;
	g_demotion_locks = 0;
	g_mission_entered = 0;
	g_mission_entry = 0;
	g_entry_ok = true;
	g_sig_rc = 0;
	build_bundle(FW_BUNDLE_IMG_TYPE_SMC_BL1, 0xC0070000ULL, 0xC0070000ULL,
		     FW_BUNDLE_IMG_TYPE_SEP_MIS, 0x1000ULL, 0x1200ULL, true);
}

ZTEST_SUITE(sep_bl1, NULL, suite_setup, before, NULL, NULL);

ZTEST(sep_bl1, test_sha256_empty)
{
	static const uint8_t expected[32] = {
		0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f,
		0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b,
		0x78, 0x52, 0xb8, 0x55};
	uint8_t out[32];

	zassert_ok(tt_fw_bundle_sha256(NULL, 0, out));
	zassert_mem_equal(out, expected, 32);
}

ZTEST(sep_bl1, test_parse_and_select_types)
{
	struct tt_fw_bundle_view view;
	const struct fw_bundle_toc_entry *smc;
	const struct fw_bundle_toc_entry *mis;
	const struct fw_bundle_toc_entry *sep_bl1;

	zassert_ok(tt_fw_bundle_parse(g_bundle, g_bundle_len, &view));
	smc = tt_fw_bundle_find_image(&view, FW_BUNDLE_IMG_TYPE_SMC_BL1);
	mis = tt_fw_bundle_find_image(&view, FW_BUNDLE_IMG_TYPE_SEP_MIS);
	sep_bl1 = tt_fw_bundle_find_image(&view, FW_BUNDLE_IMG_TYPE_SEP_BL1);

	zassert_not_null(smc);
	zassert_not_null(mis);
	zassert_is_null(sep_bl1);
	zassert_equal(smc->load_addr, 0xC0070000ULL);
	zassert_equal(tt_fw_bundle_entry_point(smc), 0xC0070000ULL);
	zassert_equal(tt_fw_bundle_entry_point(mis), 0x1200ULL);
	zassert_equal(mis->load_addr, 0x1000ULL);
}

ZTEST(sep_bl1, test_validate_success_nonsecure)
{
	struct tt_fw_bundle_view view;
	struct tt_fw_bundle_device_info d = dev_ns();

	zassert_ok(tt_fw_bundle_validate(g_bundle, g_bundle_len, &d, NULL, &view));
}

ZTEST(sep_bl1, test_malformed_toc_id)
{
	struct tt_fw_bundle_view view;
	struct fw_bundle_toc *toc = (struct fw_bundle_toc *)(g_bundle + TT_FW_BUNDLE_MANIFEST_V1_SIZE);

	toc->toc_identifier = 0xdeadbeefU;
	zassert_equal(tt_fw_bundle_parse(g_bundle, g_bundle_len, &view), -EINVAL);
}

ZTEST(sep_bl1, test_invalid_image_type_still_parses)
{
	struct tt_fw_bundle_view view;
	struct fw_bundle_toc *toc = (struct fw_bundle_toc *)(g_bundle + TT_FW_BUNDLE_MANIFEST_V1_SIZE);
	struct fw_bundle_manifest *m = (struct fw_bundle_manifest *)g_bundle;

	toc->entries[0].type = 0x1111ULL;
	fill_hashes(m, toc, g_bundle + TT_FW_BUNDLE_MANIFEST_V1_SIZE, g_bundle_len);
	zassert_ok(tt_fw_bundle_parse(g_bundle, g_bundle_len, &view));
	zassert_is_null(tt_fw_bundle_find_image(&view, FW_BUNDLE_IMG_TYPE_SMC_BL1));
}

ZTEST(sep_bl1, test_hash_failure)
{
	struct tt_fw_bundle_view view;
	struct tt_fw_bundle_device_info d = dev_ns();

	g_bundle[TT_FW_BUNDLE_MANIFEST_V1_SIZE + 64] ^= 0xFF;
	zassert_equal(tt_fw_bundle_validate(g_bundle, g_bundle_len, &d, NULL, &view), -EBADMSG);
}

ZTEST(sep_bl1, test_secure_boot_requires_signature_ops)
{
	struct tt_fw_bundle_view view;
	struct tt_fw_bundle_device_info d = dev_ns();

	d.secure_boot = true;
	zassert_equal(tt_fw_bundle_validate(g_bundle, g_bundle_len, &d, NULL, &view), -ENOTSUP);
}

ZTEST(sep_bl1, test_signature_failure)
{
	struct tt_fw_bundle_view view;
	struct sep_bl1_ctx ctx = make_ctx(true);

	g_sig_rc = -EBADMSG;
	zassert_equal(tt_fw_bundle_validate(g_bundle, g_bundle_len, &ctx.device, &ctx.auth, &view),
		      -EBADMSG);
}

ZTEST(sep_bl1, test_lifecycle_reject)
{
	struct tt_fw_bundle_view view;
	struct tt_fw_bundle_device_info d = dev_ns();

	d.secure_boot = true;
	d.lifecycle.u32_all = 0;
	d.lifecycle.f.rma_sip = 1;
	zassert_equal(tt_fw_bundle_validate(g_bundle, g_bundle_len, &d, NULL, &view), -EPERM);
}

ZTEST(sep_bl1, test_security_version_rollback)
{
	struct tt_fw_bundle_view view;
	struct tt_fw_bundle_device_info d = dev_ns();
	struct tt_fw_bundle_auth_ops auth = {.verify_manifest = verify_ok};

	d.secure_boot = true;
	d.hw_security_version = 99;
	zassert_equal(tt_fw_bundle_validate(g_bundle, g_bundle_len, &d, &auth, &view), -EPERM);
}

ZTEST(sep_bl1, test_key_revoked)
{
	struct tt_fw_bundle_view view;
	struct tt_fw_bundle_device_info d = dev_ns();
	struct tt_fw_bundle_auth_ops auth = {.verify_manifest = verify_ok};

	d.secure_boot = true;
	d.revoked_key_index_mask = BIT(0);
	zassert_equal(tt_fw_bundle_validate(g_bundle, g_bundle_len, &d, &auth, &view), -EPERM);
}

ZTEST(sep_bl1, test_missing_smc_bl1)
{
	struct tt_fw_bundle_view view;
	struct sep_bl1_ctx ctx = make_ctx(false);
	const struct fw_bundle_toc_entry *smc = (const void *)1;
	const struct fw_bundle_toc_entry *mis;

	build_bundle(FW_BUNDLE_IMG_TYPE_SEP_MIS, 0x1000ULL, 0x1200ULL, 0, 0, 0, false);
	zassert_equal(sep_bl1_validate_staged_bundle(g_bundle, g_bundle_len, &ctx, &view, &smc, &mis),
		      -ENOENT);
	zassert_is_null(smc);
	zassert_equal(SEP_BL1_STATUS_EXTRACT_VALUE(g_last_status), SEP_BL1_ERROR_MISSING_IMAGE);
}

ZTEST(sep_bl1, test_handshake_and_mission_handoff)
{
	struct tt_fw_bundle_view view;
	struct sep_bl1_ctx ctx = make_ctx(false);
	uint32_t scratch = SEP_BL1_BUNDLE_READY_FOR_VALIDATION_BIT;

	zassert_ok(sep_bl1_service_validation_scratch(&scratch, g_bundle, g_bundle_len, &ctx, &view));
	zassert_true((scratch & SEP_BL1_BUNDLE_VALIDATED_BIT) != 0U);

	zassert_equal(sep_bl1_enter_mission_firmware(&view, &ctx), -EIO);
	zassert_equal(g_mission_entered, 1);
	zassert_equal(g_mission_entry, 0x1200ULL);
	zassert_equal(g_last_status, sep_bl1_status_word(SEP_BL1_MSG_ERROR, SEP_BL1_STATUS_UNEXPECTED_EXIT));
}

ZTEST(sep_bl1, test_invalid_entry_point)
{
	struct tt_fw_bundle_view view;
	struct sep_bl1_ctx ctx = make_ctx(false);

	zassert_ok(sep_bl1_validate_staged_bundle(g_bundle, g_bundle_len, &ctx, &view, NULL, NULL));
	g_entry_ok = false;
	zassert_equal(sep_bl1_enter_mission_firmware(&view, &ctx), -EINVAL);
	zassert_equal(g_mission_entered, 0);
}

ZTEST(sep_bl1, test_handshake_idle_when_not_ready)
{
	struct tt_fw_bundle_view view = {0};
	struct sep_bl1_ctx ctx = make_ctx(false);
	uint32_t scratch = 0;

	zassert_ok(sep_bl1_service_validation_scratch(&scratch, g_bundle, g_bundle_len, &ctx, &view));
	zassert_equal(scratch, 0U);
	zassert_is_null(view.manifest);
}

ZTEST(sep_bl1, test_demotion_without_hw_is_not_success)
{
	struct tt_fw_bundle_view view;
	struct sep_bl1_ctx ctx = make_ctx(false);
	struct fw_bundle_manifest *m = (struct fw_bundle_manifest *)g_bundle;
	struct fw_bundle_toc *toc = (struct fw_bundle_toc *)(g_bundle + TT_FW_BUNDLE_MANIFEST_V1_SIZE);

	m->authenticated_flags.f.bl1_demotion = 1;
	fill_hashes(m, toc, g_bundle + TT_FW_BUNDLE_MANIFEST_V1_SIZE, g_bundle_len);
	ctx.hw.lock_demotion = NULL;
	zassert_equal(sep_bl1_validate_staged_bundle(g_bundle, g_bundle_len, &ctx, &view, NULL, NULL),
		      -ENOTSUP);
}

ZTEST(sep_bl1, test_secver_update_path)
{
	struct tt_fw_bundle_view view;
	struct sep_bl1_ctx ctx = make_ctx(false);
	struct fw_bundle_manifest *m = (struct fw_bundle_manifest *)g_bundle;
	struct fw_bundle_toc *toc = (struct fw_bundle_toc *)(g_bundle + TT_FW_BUNDLE_MANIFEST_V1_SIZE);

	m->authenticated_flags.f.security_version_update = 1;
	m->security_version = 7;
	fill_hashes(m, toc, g_bundle + TT_FW_BUNDLE_MANIFEST_V1_SIZE, g_bundle_len);
	zassert_ok(sep_bl1_validate_staged_bundle(g_bundle, g_bundle_len, &ctx, &view, NULL, NULL));
	zassert_equal(g_secver, 7);
}

static uint8_t g_smc_dest[IMG_LEN];
static uint8_t g_serdes_dest[IMG_LEN];
static uint64_t g_reset_vec;
static uint64_t g_reset_ctrl = 0x10FULL;

static int test_copy_to(uint64_t dest, const void *src, size_t len)
{
	if (dest == 0xC0150000ULL) {
		memcpy(g_smc_dest, src, len);
		return 0;
	}
	if (dest == 0x22400000ULL) {
		memcpy(g_serdes_dest, src, len);
		return 0;
	}
	return -EINVAL;
}

static int test_write64(uint64_t addr, uint64_t val)
{
	if (addr == 0xC0010000ULL) {
		g_reset_vec = val;
		return 0;
	}
	if (addr == 0xC0010020ULL) {
		g_reset_ctrl = val;
		return 0;
	}
	return -EINVAL;
}

static int test_read64(uint64_t addr, uint64_t *val)
{
	if (addr != 0xC0010020ULL || val == NULL) {
		return -EINVAL;
	}
	*val = g_reset_ctrl;
	return 0;
}

ZTEST(sep_bl1, test_load_bl0p5_serdes_and_start_smc)
{
	struct tt_fw_bundle_view view;
	struct sep_bl1_ctx ctx = make_ctx(false);
	const struct fw_bundle_toc_entry *bl0p5 = NULL;
	int placed = 0;

	memset(g_smc_dest, 0, sizeof(g_smc_dest));
	memset(g_serdes_dest, 0, sizeof(g_serdes_dest));
	g_reset_ctrl = 0x10FULL;
	g_reset_vec = 0;

	build_bundle(FW_BUNDLE_IMG_TYPE_SMC_BL0P5, 0xC0150000ULL, 0xC0150100ULL, 0x0ULL,
		     0x22400000ULL, 0x0ULL, true);

	zassert_ok(tt_fw_bundle_parse(g_bundle, g_bundle_len, &view));
	ctx.hw.copy_to = test_copy_to;
	ctx.hw.write64 = test_write64;
	ctx.hw.read64 = test_read64;

	zassert_ok(sep_bl1_load_smc_bl0p5(&view, &ctx, &bl0p5));
	zassert_not_null(bl0p5);
	zassert_equal(g_smc_dest[0], 0xA1);
	zassert_equal(g_smc_dest[IMG_LEN - 1], 0xA1);

	zassert_ok(sep_bl1_place_serdes(&view, &ctx, 0x22400000ULL, 0x8000ULL, &placed));
	zassert_equal(placed, 1);
	zassert_equal(g_serdes_dest[0], 0xB2);

	zassert_ok(sep_bl1_start_smc(bl0p5, &ctx, 0xC0010000ULL, 0xC0010020ULL));
	zassert_equal(g_reset_vec, 0xC0150100ULL);
	zassert_true((g_reset_ctrl & BIT64(0)) != 0U);
}

ZTEST(sep_bl1, test_unsecured_bun2_ack)
{
	struct tt_fw_bundle_view view;
	struct sep_bl1_ctx ctx = make_ctx(false);
	uint32_t scratch = SEP_BL1_BUNDLE_READY_FOR_VALIDATION_BIT;

	zassert_ok(sep_bl1_ack_bun2_unsecured(&scratch, g_bundle, g_bundle_len, &ctx, &view));
	zassert_true((scratch & SEP_BL1_BUNDLE_VALIDATED_BIT) != 0U);
	zassert_not_null(view.toc);
}
