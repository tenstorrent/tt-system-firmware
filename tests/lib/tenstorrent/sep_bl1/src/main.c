/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <tenstorrent/sep_bl1.h>

#define MAX_BUNDLE    2048
#define IMG_LEN       8
#define MANIFEST_SIZE sizeof(struct fw_bundle_manifest)

#define BL0P5_LOAD  0xC0150000ULL
#define BL0P5_ENTRY 0xC0150100ULL

static uint8_t g_bundle[MAX_BUNDLE];
static size_t g_bundle_len;
static uint32_t g_last_status;

static uint8_t g_smc_dest[IMG_LEN];
static uint64_t g_reset_vec;
static uint64_t g_reset_ctrl;

static void post_status(uint32_t word)
{
	g_last_status = word;
}

static int test_copy_to(uint64_t dest, const void *src, size_t len)
{
	if (dest != BL0P5_LOAD || len > sizeof(g_smc_dest)) {
		return -EINVAL;
	}
	memcpy(g_smc_dest, src, len);
	return 0;
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

static struct sep_bl1_ctx make_ctx(void)
{
	struct sep_bl1_ctx ctx = {0};

	ctx.hw.post_status = post_status;
	ctx.hw.copy_to = test_copy_to;
	ctx.hw.write64 = test_write64;
	ctx.hw.read64 = test_read64;
	return ctx;
}

static struct fw_bundle_toc *bundle_toc(void)
{
	return (struct fw_bundle_toc *)(g_bundle + MANIFEST_SIZE);
}

/* BUN1 layout: SEP BL1 at TOC[0], SMC BL0P5 at TOC[1]. */
static void build_bundle(uint64_t type1)
{
	struct fw_bundle_manifest *m = (struct fw_bundle_manifest *)g_bundle;
	struct fw_bundle_toc *toc = bundle_toc();
	uint8_t *payload = g_bundle + MANIFEST_SIZE;
	uint64_t img0_off = sizeof(*toc) + 2U * sizeof(toc->entries[0]);
	uint64_t img1_off = img0_off + IMG_LEN;
	uint64_t payload_len = img1_off + IMG_LEN;

	memset(g_bundle, 0, sizeof(g_bundle));
	m->manifest_identifier = MANIFEST_ID_BL1;
	m->manifest_version_major = 1;
	m->manifest_length = MANIFEST_SIZE;
	m->payload_offset = (int64_t)MANIFEST_SIZE;
	m->payload_length = payload_len;

	toc->toc_identifier = FW_BUNDLE_TOC_ID;
	toc->toc_version_major = 1;
	toc->image_count = 2;
	toc->payload_length = payload_len;

	toc->entries[0].type = FW_BUNDLE_IMG_TYPE_SEP_BL1;
	toc->entries[0].offset = img0_off;
	toc->entries[0].length = IMG_LEN;
	memset(payload + img0_off, 0xA1, IMG_LEN);

	toc->entries[1].type = type1;
	toc->entries[1].offset = img1_off;
	toc->entries[1].length = IMG_LEN;
	toc->entries[1].load_addr = BL0P5_LOAD;
	toc->entries[1].entry_point = BL0P5_ENTRY;
	memset(payload + img1_off, 0xB2, IMG_LEN);

	g_bundle_len = MANIFEST_SIZE + (size_t)payload_len;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	g_last_status = 0;
	g_reset_vec = 0;
	g_reset_ctrl = 0x10FULL;
	memset(g_smc_dest, 0, sizeof(g_smc_dest));
	build_bundle(FW_BUNDLE_IMG_TYPE_SMC_BL0P5);
}

ZTEST_SUITE(sep_bl1, NULL, NULL, before, NULL, NULL);

ZTEST(sep_bl1, test_bundle_toc_found)
{
	zassert_equal(sep_bl1_bundle_toc(g_bundle, g_bundle_len), bundle_toc());
}

ZTEST(sep_bl1, test_bundle_toc_rejects_non_bundle)
{
	struct fw_bundle_manifest *m = (struct fw_bundle_manifest *)g_bundle;

	zassert_is_null(sep_bl1_bundle_toc(g_bundle, MANIFEST_SIZE - 1U));

	bundle_toc()->toc_identifier = 0xdeadbeefU;
	zassert_is_null(sep_bl1_bundle_toc(g_bundle, g_bundle_len));

	build_bundle(FW_BUNDLE_IMG_TYPE_SMC_BL0P5);
	m->payload_offset = -16;
	zassert_is_null(sep_bl1_bundle_toc(g_bundle, g_bundle_len));

	m->payload_offset = (int64_t)g_bundle_len;
	zassert_is_null(sep_bl1_bundle_toc(g_bundle, g_bundle_len));
}

ZTEST(sep_bl1, test_bundle_toc_rejects_entries_past_buffer)
{
	bundle_toc()->image_count = 100;
	zassert_is_null(sep_bl1_bundle_toc(g_bundle, g_bundle_len));
}

ZTEST(sep_bl1, test_load_bl0p5_and_start_smc)
{
	struct sep_bl1_ctx ctx = make_ctx();
	const struct fw_bundle_toc_entry *bl0p5 = NULL;

	zassert_ok(sep_bl1_load_smc_bl0p5(g_bundle, g_bundle_len, &ctx, &bl0p5));
	zassert_equal(bl0p5, &bundle_toc()->entries[1]);
	zassert_equal(g_smc_dest[0], 0xB2);
	zassert_equal(g_smc_dest[IMG_LEN - 1], 0xB2);

	/* The entry point is the SMC's own address and goes to the vector as-is. */
	zassert_ok(sep_bl1_start_smc(bl0p5, &ctx, 0xC0010000ULL, 0xC0010020ULL));
	zassert_equal(g_reset_vec, BL0P5_ENTRY);
	zassert_true((g_reset_ctrl & BIT64(0)) != 0U);
}

ZTEST(sep_bl1, test_load_bl0p5_wrong_type_in_slot)
{
	struct sep_bl1_ctx ctx = make_ctx();
	const struct fw_bundle_toc_entry *bl0p5 = NULL;

	build_bundle(FW_BUNDLE_IMG_TYPE_SMC_BL1);
	zassert_equal(sep_bl1_load_smc_bl0p5(g_bundle, g_bundle_len, &ctx, &bl0p5), -ENOENT);
	zassert_is_null(bl0p5);
	zassert_equal(SEP_BL1_STATUS_EXTRACT_VALUE(g_last_status), SEP_BL1_ERROR_MISSING_IMAGE);
}

ZTEST(sep_bl1, test_load_bl0p5_missing_slot)
{
	struct sep_bl1_ctx ctx = make_ctx();

	bundle_toc()->image_count = 1;
	zassert_equal(sep_bl1_load_smc_bl0p5(g_bundle, g_bundle_len, &ctx, NULL), -ENOENT);
}

ZTEST(sep_bl1, test_load_bl0p5_image_past_buffer)
{
	struct sep_bl1_ctx ctx = make_ctx();

	bundle_toc()->entries[1].length = g_bundle_len;
	zassert_equal(sep_bl1_load_smc_bl0p5(g_bundle, g_bundle_len, &ctx, NULL), -EINVAL);
	zassert_equal(g_smc_dest[0], 0);
}

ZTEST(sep_bl1, test_start_smc_requires_mmio_ops)
{
	struct sep_bl1_ctx ctx = make_ctx();

	ctx.hw.write64 = NULL;
	zassert_equal(
		sep_bl1_start_smc(&bundle_toc()->entries[1], &ctx, 0xC0010000ULL, 0xC0010020ULL),
		-ENOTSUP);
	zassert_equal(g_reset_vec, 0);
}

#define BL0_STATE_SIZE 104U

static uint32_t g_bl0_state[BL0_STATE_SIZE / 4U];

static const uint8_t *make_bl0_state(uint32_t manifest_addr)
{
	memset(g_bl0_state, 0, sizeof(g_bl0_state));
	g_bl0_state[0] = SEP_BL1_BL0_STATE_MAGIC;
	g_bl0_state[1] = manifest_addr;
	g_bl0_state[ARRAY_SIZE(g_bl0_state) - 2] = BL0_STATE_SIZE;
	g_bl0_state[ARRAY_SIZE(g_bl0_state) - 1] = SEP_BL1_BL0_STATE_MAGIC;
	return (const uint8_t *)g_bl0_state + sizeof(g_bl0_state);
}

ZTEST(sep_bl1, test_bl0_state_manifest_addr)
{
	const uint8_t *end = make_bl0_state(0xC1800000U);
	uint32_t addr = 0;

	zassert_ok(sep_bl1_bl0_manifest_addr(end, &addr));
	zassert_equal(addr, 0xC1800000U);
}

ZTEST(sep_bl1, test_bl0_state_rejects_bad_block)
{
	const uint8_t *end;
	uint32_t addr = 0;

	end = make_bl0_state(0xC1800000U);
	g_bl0_state[ARRAY_SIZE(g_bl0_state) - 1] = 0;
	zassert_equal(sep_bl1_bl0_manifest_addr(end, &addr), -ENOENT);

	end = make_bl0_state(0xC1800000U);
	g_bl0_state[0] = 0;
	zassert_equal(sep_bl1_bl0_manifest_addr(end, &addr), -ENOENT);

	end = make_bl0_state(0xC1800000U);
	g_bl0_state[ARRAY_SIZE(g_bl0_state) - 2] = 1024U;
	zassert_equal(sep_bl1_bl0_manifest_addr(end, &addr), -ENOENT);
	zassert_equal(addr, 0U);
}

static uint8_t g_stage[4096];
static uint8_t g_bl0_payload[MAX_BUNDLE];
static struct fw_bundle_manifest g_bl0_manifest;

/* What SEP BL0 leaves: manifest on its own, payload_offset pointing elsewhere. */
static size_t split_bundle(void)
{
	size_t plen = g_bundle_len - MANIFEST_SIZE;

	memcpy(&g_bl0_manifest, g_bundle, sizeof(g_bl0_manifest));
	g_bl0_manifest.payload_offset = 0x10000;
	memcpy(g_bl0_payload, g_bundle + MANIFEST_SIZE, plen);
	return plen;
}

static void assert_staged_matches_bundle(void)
{
	const struct fw_bundle_manifest *m = (const struct fw_bundle_manifest *)g_stage;

	zassert_equal(m->payload_offset, (int64_t)MANIFEST_SIZE);
	zassert_not_null(sep_bl1_bundle_toc(g_stage, sizeof(g_stage)));
	zassert_mem_equal(g_stage + MANIFEST_SIZE, g_bundle + MANIFEST_SIZE,
			  g_bundle_len - MANIFEST_SIZE);
}

ZTEST(sep_bl1, test_stage_bundle_from_split_bl0_layout)
{
	struct sep_bl1_ctx ctx = make_ctx();

	split_bundle();
	memset(g_stage, 0, sizeof(g_stage));
	zassert_ok(sep_bl1_stage_bundle(&g_bl0_manifest, g_bl0_payload, g_stage, sizeof(g_stage),
					&ctx));
	assert_staged_matches_bundle();
}

ZTEST(sep_bl1, test_stage_bundle_payload_already_in_window)
{
	struct sep_bl1_ctx ctx = make_ctx();
	size_t plen = split_bundle();

	/* BL0 copied the payload to the start of the window itself. */
	memset(g_stage, 0, sizeof(g_stage));
	memcpy(g_stage, g_bl0_payload, plen);
	zassert_ok(sep_bl1_stage_bundle(&g_bl0_manifest, g_stage, g_stage, sizeof(g_stage), &ctx));
	assert_staged_matches_bundle();
}

ZTEST(sep_bl1, test_stage_bundle_window_too_small)
{
	struct sep_bl1_ctx ctx = make_ctx();

	split_bundle();
	zassert_equal(sep_bl1_stage_bundle(&g_bl0_manifest, g_bl0_payload, g_stage,
					   g_bundle_len - 1U, &ctx),
		      -EINVAL);
	zassert_equal(SEP_BL1_STATUS_EXTRACT_VALUE(g_last_status), SEP_BL1_ERROR_VALIDATE);
}

ZTEST(sep_bl1, test_unsecured_bun2_ack)
{
	struct sep_bl1_ctx ctx = make_ctx();
	uint32_t scratch = SEP_BL1_BUNDLE_READY_FOR_VALIDATION_BIT;

	zassert_ok(sep_bl1_ack_bun2_unsecured(&scratch, g_bundle, g_bundle_len, &ctx));
	zassert_true((scratch & SEP_BL1_BUNDLE_VALIDATED_BIT) != 0U);
}

ZTEST(sep_bl1, test_unsecured_bun2_ack_idle_when_not_ready)
{
	struct sep_bl1_ctx ctx = make_ctx();
	uint32_t scratch = 0;

	zassert_ok(sep_bl1_ack_bun2_unsecured(&scratch, g_bundle, g_bundle_len, &ctx));
	zassert_equal(scratch, 0U);
}

ZTEST(sep_bl1, test_unsecured_bun2_ack_rejects_non_bundle)
{
	struct sep_bl1_ctx ctx = make_ctx();
	uint32_t scratch = SEP_BL1_BUNDLE_READY_FOR_VALIDATION_BIT;

	bundle_toc()->toc_identifier = 0;
	zassert_equal(sep_bl1_ack_bun2_unsecured(&scratch, g_bundle, g_bundle_len, &ctx), -EINVAL);
	zassert_equal(scratch, SEP_BL1_BUNDLE_READY_FOR_VALIDATION_BIT);
	zassert_equal(SEP_BL1_STATUS_EXTRACT_VALUE(g_last_status), SEP_BL1_ERROR_VALIDATE);
}
