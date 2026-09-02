/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for the bh_fwtable ccfgovr (A/B config override) mechanism.
 *
 * Suite: bh_fwtable_ccfgovr (built with CONFIG_BH_FWTABLE_CCFGOVR=y)
 *   Happy paths:
 *     - test_happy_path_active_bank_applies: valid bank applies tdp_limit
 *     - test_kernel_throttler_override_applies: kernel-throttler fields apply
 *     - test_gddr_therm_trip_override_applies: gddr therm-trip feature applies
 *     - test_eth_speed_override_applies: eth_speed_override applies
 *     - test_eth_speed_override_of_zero_asks_for_auto_train: explicit 0 is auto
 *   Sequence selection:
 *     - test_newer_seq_in_bank_b_wins: higher seq in B wins
 *     - test_newer_seq_in_bank_a_wins: higher seq in A wins
 *     - test_equal_seq_bank_a_wins: equal-seq tie-break favours A
 *     - test_seq_wraparound_selects_newer: signed wrap-around comparison
 *   Fallback / integrity:
 *     - test_protobuf_decode_fails: undecodable body is ignored
 *     - test_decode_failure_falls_back_to_older_bank: fall back after decode fail
 *     - test_body_crc_mismatch: stored CRC mismatch is rejected
 *     - test_body_bit_flip_rejected: body corruption is rejected
 *     - test_header_field_in_crc_range_flip_rejected: header corruption rejected
 *     - test_active_invalid_falls_back_to_inactive: bad A falls back to B
 *     - test_equal_seq_bad_crc_falls_back: equal-seq bad CRC falls back
 *     - test_both_banks_invalid_no_override: no valid bank -> no override
 *   Header validation:
 *     - test_wrong_magic_rejected
 *     - test_seq_erased_rejected
 *     - test_wrong_version_rejected
 *     - test_body_len_unaligned_rejected
 *     - test_body_len_too_large_rejected
 *     - test_body_len_at_max_boundary_applies
 *     - test_image_size_smaller_than_header_rejected
 *     - test_missing_bank_tag_uses_other_bank
 *   Empty body:
 *     - test_empty_body_is_noop
 *     - test_empty_newest_bank_stops_search
 *   Merge semantics / security:
 *     - test_reserved_field_is_ignored: reserved fw_bundle_version not forgeable
 *     - test_unknown_field_is_skipped
 *     - test_partial_override_preserves_other_fields
 *     - test_chip_limits_without_tdp_preserves_tdp
 *     - test_explicit_zero_value_applies
 *     - test_all_allowlisted_fields_apply_together
 *
 * Suite: bh_fwtable_ccfgovr_disabled (built with CONFIG_BH_FWTABLE_CCFGOVR=n)
 *     - test_feature_compiled_out: feature is compiled out and links cleanly
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/misc/bh_fwtable.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef CONFIG_BH_FWTABLE_CCFGOVR

#include <pb_encode.h>
#include <tenstorrent/tt_boot_fs.h>

#include "fw_table_override.pb.h"

/* Mirror of drivers/misc/bh_fwtable/ccfgovr.h. */
#define CCFGOVR_MAGIC       0x564F4343U
#define CCFGOVR_HDR_VERSION 0U
#define CCFGOVR_SEQ_ERASED  0xFFFFFFFFU

struct ccfgovr_bank_hdr {
	uint32_t magic;
	uint32_t seq;
	uint32_t body_len;
	uint32_t version;
	uint32_t cksum;
};

/* Maximum protobuf body the driver will decode (see CCFGOVR_DECODE_BODY_MAX). */
#define CCFGOVR_DECODE_BODY_MAX 512U

#define FLASH_NODE   DT_NODELABEL(flashcontroller0)
#define FWTABLE_NODE DT_NODELABEL(fwtable)

static const struct device *const flash_dev = DEVICE_DT_GET(FLASH_NODE);
static const struct device *const fwtable_dev = DEVICE_DT_GET(FWTABLE_NODE);

#define BANK_A_ADDR 0x00010000U
#define BANK_B_ADDR 0x00011000U
#define BANK_SIZE   0x00001000U

#define FD_AREA_ERASE_SIZE 0x1000U

#define DEFAULT_TDP_LIMIT 150U
/* Stands in for a cmfwcfg that pins the link speed, as the Galaxy tables do. */
#define DEFAULT_ETH_SPEED 200U

static void write_fd(size_t slot, const char *tag, uint32_t spi_addr, uint32_t image_size)
{
	tt_boot_fs_fd fd;

	memset(&fd, 0, sizeof(fd));
	fd.spi_addr = spi_addr;
	fd.flags.f.image_size = image_size;
	memcpy(fd.image_tag, tag, MIN(strlen(tag), sizeof(fd.image_tag)));
	fd.fd_crc = tt_boot_fs_cksum(0, (const uint8_t *)&fd, sizeof(fd) - sizeof(fd.fd_crc));

	int rc = flash_write(flash_dev, TT_BOOT_FS_FD_HEAD_ADDR + slot * sizeof(fd), &fd,
			     sizeof(fd));
	zassert_equal(rc, 0, "flash_write slot %zu failed with %d", slot, rc);
}

static size_t encode_override_msg(const FwTableOverride *ovr, uint8_t *out, size_t out_size)
{
	pb_ostream_t stream = pb_ostream_from_buffer(out, out_size);

	zassert_true(pb_encode_ex(&stream, FwTableOverride_fields, ovr, PB_ENCODE_NULLTERMINATED),
		     "pb_encode_ex failed: %s", PB_GET_ERROR(&stream));

	size_t total = stream.bytes_written;

	/* Pad to 4 byte alignment for ccfgovr header requirement */
	while ((total % 4U) != 0U) {
		zassert_true(total < out_size, "padded body overruns buffer");
		out[total++] = 0x00U;
	}
	return total;
}

/*
 * Flexible override encoder. Each has_* flag controls whether the matching
 * field (and its parent sub-message) is present in the encoded message. The
 * body is zero-padded to a 4-byte boundary as required by the ccfgovr framing.
 */
static size_t encode_override(uint8_t *out, size_t out_size, bool has_tdp, uint32_t tdp,
			      bool has_stop_freq, uint32_t stop_freq, bool has_ktf, bool ktf_en)
{
	FwTableOverride ovr = FwTableOverride_init_zero;

	if (has_tdp || has_stop_freq) {
		ovr.has_chip_limits = true;
		if (has_tdp) {
			ovr.chip_limits.has_tdp_limit = true;
			ovr.chip_limits.tdp_limit = tdp;
		}
		if (has_stop_freq) {
			ovr.chip_limits.has_kernel_throttler_stop_nops_freq = true;
			ovr.chip_limits.kernel_throttler_stop_nops_freq = stop_freq;
		}
	}

	if (has_ktf) {
		ovr.has_feature_enable = true;
		ovr.feature_enable.has_kernel_throttler_at_floor_en = true;
		ovr.feature_enable.kernel_throttler_at_floor_en = ktf_en;
	}

	return encode_override_msg(&ovr, out, out_size);
}

static size_t encode_tdp_limit_override(uint8_t *out, size_t out_size, uint32_t value)
{
	return encode_override(out, out_size, true, value, false, 0, false, false);
}

static size_t encode_kernel_throttler_override(uint8_t *out, size_t out_size, bool enabled,
					       uint32_t stop_freq)
{
	return encode_override(out, out_size, false, 0, true, stop_freq, true, enabled);
}

static size_t encode_eth_speed_override(uint8_t *out, size_t out_size, uint32_t speed)
{
	FwTableOverride ovr = FwTableOverride_init_zero;

	ovr.has_eth_property_table = true;
	ovr.eth_property_table.has_eth_speed_override = true;
	ovr.eth_property_table.eth_speed_override = speed;

	return encode_override_msg(&ovr, out, out_size);
}

static size_t encode_gddr_therm_trip_override(uint8_t *out, size_t out_size, bool enabled)
{
	FwTableOverride ovr = FwTableOverride_init_zero;

	ovr.has_feature_enable = true;
	ovr.feature_enable.has_gddr_therm_trip_en = true;
	ovr.feature_enable.gddr_therm_trip_en = enabled;

	return encode_override_msg(&ovr, out, out_size);
}

/* Compute the ccfgovr CRC exactly like the driver: header[0..cksum) || body[]. */
static uint32_t ccfgovr_calc_crc(const struct ccfgovr_bank_hdr *hdr, const uint8_t *body,
				 size_t body_len)
{
	uint32_t crc = crc32_ieee_update(0, (const uint8_t *)hdr,
					 offsetof(struct ccfgovr_bank_hdr, cksum));

	return crc32_ieee_update(crc, body, body_len);
}

static void write_bank(uint32_t addr, struct ccfgovr_bank_hdr *hdr, const uint8_t *body,
		       size_t body_len)
{
	int rc = flash_erase(flash_dev, addr, BANK_SIZE);

	zassert_equal(rc, 0, "flash_erase bank@0x%x failed with %d", addr, rc);

	hdr->body_len = (uint32_t)body_len;
	if (hdr->cksum == 0) {
		hdr->cksum = ccfgovr_calc_crc(hdr, body, body_len);
	}

	rc = flash_write(flash_dev, addr, hdr, sizeof(*hdr));
	zassert_equal(rc, 0, "flash_write hdr@0x%x failed with %d", addr, rc);

	if (body_len > 0) {
		rc = flash_write(flash_dev, addr + (uint32_t)sizeof(*hdr), body, body_len);
		zassert_equal(rc, 0, "flash_write body@0x%x failed with %d",
			      addr + (uint32_t)sizeof(*hdr), rc);
	}
}

/* Write a bank using a raw, hand-encoded protobuf body (must be 4-byte aligned). */
static void write_raw_bank(uint32_t addr, uint32_t seq, const uint8_t *raw, size_t raw_len)
{
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = seq};

	write_bank(addr, &hdr, raw, raw_len);
}

static void erase_both_banks(void)
{
	int rc;

	rc = flash_erase(flash_dev, BANK_A_ADDR, BANK_SIZE);
	zassert_equal(rc, 0, "flash_erase BANK_A failed with %d", rc);

	rc = flash_erase(flash_dev, BANK_B_ADDR, BANK_SIZE);
	zassert_equal(rc, 0, "flash_erase BANK_B failed with %d", rc);
}

/* (Re)write the boot-fs descriptors for the two ccfgovr banks. */
static void setup_fds(bool write_a, uint32_t a_image_size, bool write_b, uint32_t b_image_size)
{
	int rc = flash_erase(flash_dev, TT_BOOT_FS_FD_HEAD_ADDR, FD_AREA_ERASE_SIZE);

	zassert_equal(rc, 0, "flash_erase failed with %d", rc);

	/* Keep valid descriptors contiguous from slot 0 so the boot-fs scan
	 * never stops early on an erased entry.
	 */
	size_t slot = 0;

	if (write_a) {
		write_fd(slot++, "ccfgovra", BANK_A_ADDR, a_image_size);
	}
	if (write_b) {
		write_fd(slot++, "ccfgovrb", BANK_B_ADDR, b_image_size);
	}
}

static void setup_fds_default(void)
{
	setup_fds(true, BANK_SIZE, true, BANK_SIZE);
}

static FwTable *fw_table(void)
{
	return (FwTable *)tt_bh_fwtable_get_fw_table(fwtable_dev);
}

static uint32_t tdp_limit(void)
{
	return fw_table()->chip_limits.tdp_limit;
}

static void reset_fwtable(void)
{
	FwTable *t = fw_table();

	memset(t, 0, sizeof(*t));
	t->chip_limits.tdp_limit = DEFAULT_TDP_LIMIT;
	t->eth_property_table.has_eth_speed_override = true;
	t->eth_property_table.eth_speed_override = DEFAULT_ETH_SPEED;
}

/**
 * @brief Setup minimal boot_fs with ccfgovr a/b as the only entries
 */
static void *suite_setup(void)
{
	zassert_true(device_is_ready(flash_dev));

	setup_fds_default();

	return NULL;
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);

	erase_both_banks();
	reset_fwtable();
}

static void after_each(void *fixture)
{
	ARG_UNUSED(fixture);

	/* Restore default descriptors in case a test rewrote them. */
	setup_fds_default();
}

ZTEST_SUITE(bh_fwtable_ccfgovr, NULL, suite_setup, before_each, after_each, NULL);

/* ------------------------------------------------------------------------- */
/* Happy paths                                                               */
/* ------------------------------------------------------------------------- */

/**
 * @brief Test good path when ccfgovr is valid and successfully applied
 */
ZTEST(bh_fwtable_ccfgovr, test_happy_path_active_bank_applies)
{
	uint8_t body[16];
	size_t body_len = encode_tdp_limit_override(body, sizeof(body), 175);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), 175, "expected override to set tdp_limit=175");
	zassert_equal(fw_table()->eth_property_table.eth_speed_override, DEFAULT_ETH_SPEED,
		      "fields absent from the override must keep their cmfwcfg value");
}

/**
 * @brief Test that the kernel-throttler-at-floor configuration can be overridden
 */
ZTEST(bh_fwtable_ccfgovr, test_kernel_throttler_override_applies)
{
	uint8_t body[16];
	size_t body_len = encode_kernel_throttler_override(body, sizeof(body), true, 800U);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_true(fw_table()->feature_enable.kernel_throttler_at_floor_en,
		     "expected override to enable kernel_throttler_at_floor_en");
	zassert_equal(fw_table()->chip_limits.kernel_throttler_stop_nops_freq, 800U,
		      "expected override to set kernel_throttler_stop_nops_freq=800");
}

/**
 * @brief Test that the GDDR thermal-trip action feature can be overridden
 */
ZTEST(bh_fwtable_ccfgovr, test_gddr_therm_trip_override_applies)
{
	uint8_t body[16];
	size_t body_len = encode_gddr_therm_trip_override(body, sizeof(body), true);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_true(fw_table()->feature_enable.gddr_therm_trip_en,
		     "expected override to enable gddr_therm_trip_en");
}

/**
 * @brief Test that the ETH training speed can be raised above the cmfwcfg value
 */
ZTEST(bh_fwtable_ccfgovr, test_eth_speed_override_applies)
{
	uint8_t body[16];
	size_t body_len = encode_eth_speed_override(body, sizeof(body), 400U);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(fw_table()->eth_property_table.eth_speed_override, 400U,
		      "expected override to set eth_speed_override=400");
}

/**
 * @brief Test that a speed of 0 overrides the cmfwcfg speed and asks for auto-train
 *
 * Both the override and the base fw table are `optional`, so an explicit 0
 * survives the merge with `has_eth_speed_override` set. `LoadEthFwCfg` writes
 * 0 into the ETH FW config, which hands the speed choice back to ERISC.
 */
ZTEST(bh_fwtable_ccfgovr, test_eth_speed_override_of_zero_asks_for_auto_train)
{
	uint8_t body[16];
	size_t body_len = encode_eth_speed_override(body, sizeof(body), 0U);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_true(fw_table()->eth_property_table.has_eth_speed_override,
		     "expected the override to set has_eth_speed_override");
	zassert_equal(fw_table()->eth_property_table.eth_speed_override, 0U,
		      "expected the override to set eth_speed_override=0 (auto)");
}

/* ------------------------------------------------------------------------- */
/* Sequence selection                                                        */
/* ------------------------------------------------------------------------- */

/**
 * @brief Test good path when ccfgovrb is newer than ccfgovra
 */
ZTEST(bh_fwtable_ccfgovr, test_newer_seq_in_bank_b_wins)
{
	uint8_t body_a[16];
	size_t body_a_len = encode_tdp_limit_override(body_a, sizeof(body_a), 77);
	struct ccfgovr_bank_hdr hdr_a = {.magic = CCFGOVR_MAGIC, .seq = 1};

	write_bank(BANK_A_ADDR, &hdr_a, body_a, body_a_len);

	uint8_t body_b[16];
	size_t body_b_len = encode_tdp_limit_override(body_b, sizeof(body_b), 88);
	struct ccfgovr_bank_hdr hdr_b = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_B_ADDR, &hdr_b, body_b, body_b_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), 88,
		      "expected bank B's newer-seq override to win (tdp_limit=88, not 77)");
}

/**
 * @brief Symmetric to the B-wins case: a newer seq in bank A must win.
 */
ZTEST(bh_fwtable_ccfgovr, test_newer_seq_in_bank_a_wins)
{
	uint8_t body_a[16];
	size_t body_a_len = encode_tdp_limit_override(body_a, sizeof(body_a), 50);
	struct ccfgovr_bank_hdr hdr_a = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr_a, body_a, body_a_len);

	uint8_t body_b[16];
	size_t body_b_len = encode_tdp_limit_override(body_b, sizeof(body_b), 60);
	struct ccfgovr_bank_hdr hdr_b = {.magic = CCFGOVR_MAGIC, .seq = 1};

	write_bank(BANK_B_ADDR, &hdr_b, body_b, body_b_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), 50, "expected bank A's newer-seq override to win");
}

/**
 * @brief Equal sequence numbers: documented tie-break favours bank A.
 */
ZTEST(bh_fwtable_ccfgovr, test_equal_seq_bank_a_wins)
{
	uint8_t body_a[16];
	size_t body_a_len = encode_tdp_limit_override(body_a, sizeof(body_a), 10);
	struct ccfgovr_bank_hdr hdr_a = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr_a, body_a, body_a_len);

	uint8_t body_b[16];
	size_t body_b_len = encode_tdp_limit_override(body_b, sizeof(body_b), 20);
	struct ccfgovr_bank_hdr hdr_b = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_B_ADDR, &hdr_b, body_b, body_b_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), 10, "expected bank A to win the equal-seq tie-break");
}

/**
 * @brief Sequence comparison must be wrap-around safe (signed delta).
 *
 * Bank A holds a near-max seq, bank B has wrapped to a small value. B is the
 * newer write and must win.
 */
ZTEST(bh_fwtable_ccfgovr, test_seq_wraparound_selects_newer)
{
	uint8_t body_a[16];
	size_t body_a_len = encode_tdp_limit_override(body_a, sizeof(body_a), 11);
	struct ccfgovr_bank_hdr hdr_a = {.magic = CCFGOVR_MAGIC, .seq = 0xFFFFFFFEU};

	write_bank(BANK_A_ADDR, &hdr_a, body_a, body_a_len);

	uint8_t body_b[16];
	size_t body_b_len = encode_tdp_limit_override(body_b, sizeof(body_b), 22);
	struct ccfgovr_bank_hdr hdr_b = {.magic = CCFGOVR_MAGIC, .seq = 1U};

	write_bank(BANK_B_ADDR, &hdr_b, body_b, body_b_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), 22, "expected wrapped seq in bank B to be treated as newer");
}

/* ------------------------------------------------------------------------- */
/* Fallback behaviour                                                        */
/* ------------------------------------------------------------------------- */

/**
 * @brief Test bad path when protobuf decode fails on the body
 */
ZTEST(bh_fwtable_ccfgovr, test_protobuf_decode_fails)
{
	uint8_t garbage_body[8];
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	memset(garbage_body, 0xFF, sizeof(garbage_body));

	write_bank(BANK_A_ADDR, &hdr, garbage_body, sizeof(garbage_body));

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), DEFAULT_TDP_LIMIT,
		      "override must not be applied when protobuf decode fails");
}

/**
 * @brief A newest bank that passes CRC but fails protobuf decode must fall back
 *        to the next-newest valid bank.
 */
ZTEST(bh_fwtable_ccfgovr, test_decode_failure_falls_back_to_older_bank)
{
	uint8_t garbage_body[8];
	struct ccfgovr_bank_hdr hdr_a = {.magic = CCFGOVR_MAGIC, .seq = 5};

	memset(garbage_body, 0xFF, sizeof(garbage_body));
	write_bank(BANK_A_ADDR, &hdr_a, garbage_body, sizeof(garbage_body));

	uint8_t body_b[16];
	size_t body_b_len = encode_tdp_limit_override(body_b, sizeof(body_b), 120);
	struct ccfgovr_bank_hdr hdr_b = {.magic = CCFGOVR_MAGIC, .seq = 4};

	write_bank(BANK_B_ADDR, &hdr_b, body_b, body_b_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), 120,
		      "expected fallback to older bank after newest bank decode failure");
}

/**
 * @brief Test bad path when the stored CRC does not match the body
 */
ZTEST(bh_fwtable_ccfgovr, test_body_crc_mismatch)
{
	uint8_t body[16];
	size_t body_len = encode_tdp_limit_override(body, sizeof(body), 140U);
	struct ccfgovr_bank_hdr hdr = {
		.magic = CCFGOVR_MAGIC,
		.seq = 2,
		.cksum = 0xDEADBEEFU,
	};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);

	zassert_equal(tdp_limit(), DEFAULT_TDP_LIMIT,
		      "override must not be applied when body CRC mismatches");
}

/**
 * @brief A single-bit flip in the body (after CRC was computed) must be rejected.
 */
ZTEST(bh_fwtable_ccfgovr, test_body_bit_flip_rejected)
{
	uint8_t body[16];
	size_t body_len = encode_tdp_limit_override(body, sizeof(body), 140U);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	hdr.body_len = (uint32_t)body_len;
	hdr.cksum = ccfgovr_calc_crc(&hdr, body, body_len);

	/* Corrupt the body after the CRC was computed. */
	body[0] ^= 0x01U;

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), DEFAULT_TDP_LIMIT,
		      "override must not be applied when a body bit is flipped");
}

/**
 * @brief A flip of a CRC-covered header field must be rejected.
 *
 * Proves the CRC covers the header (not just the body): the seq is mutated
 * after the CRC is computed, so the stored CRC no longer matches.
 */
ZTEST(bh_fwtable_ccfgovr, test_header_field_in_crc_range_flip_rejected)
{
	uint8_t body[16];
	size_t body_len = encode_tdp_limit_override(body, sizeof(body), 141U);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	hdr.body_len = (uint32_t)body_len;
	hdr.cksum = ccfgovr_calc_crc(&hdr, body, body_len);

	/* Mutate a CRC-covered header field after computing the CRC. seq=3 is
	 * still plausible, so only the CRC check can reject it.
	 */
	hdr.seq = 3;

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), DEFAULT_TDP_LIMIT,
		      "override must not be applied when a CRC-covered header field changes");
}

/**
 * @brief Test bad path when ccfgovra is invalid and ccfgovrb is valid
 */
ZTEST(bh_fwtable_ccfgovr, test_active_invalid_falls_back_to_inactive)
{
	/* ccfgovra has wrong checksum */
	uint8_t body_a[16];
	size_t body_a_len = encode_tdp_limit_override(body_a, sizeof(body_a), 999);
	struct ccfgovr_bank_hdr hdr_a = {
		.magic = CCFGOVR_MAGIC,
		.seq = 5,
		.cksum = 0x11111111U,
	};

	write_bank(BANK_A_ADDR, &hdr_a, body_a, body_a_len);

	/* ccfgovrb is valid */
	uint8_t body_b[16];
	size_t body_b_len = encode_tdp_limit_override(body_b, sizeof(body_b), 120);
	struct ccfgovr_bank_hdr hdr_b = {.magic = CCFGOVR_MAGIC, .seq = 4};

	write_bank(BANK_B_ADDR, &hdr_b, body_b, body_b_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), 120, "expected fallback to inactive bank's override");
}

/**
 * @brief With equal seq, a bad CRC in the (tie-break preferred) bank A must
 *        fall back to the still-valid bank B.
 */
ZTEST(bh_fwtable_ccfgovr, test_equal_seq_bad_crc_falls_back)
{
	uint8_t body_a[16];
	size_t body_a_len = encode_tdp_limit_override(body_a, sizeof(body_a), 70);
	struct ccfgovr_bank_hdr hdr_a = {
		.magic = CCFGOVR_MAGIC,
		.seq = 3,
		.cksum = 0x22222222U,
	};

	write_bank(BANK_A_ADDR, &hdr_a, body_a, body_a_len);

	uint8_t body_b[16];
	size_t body_b_len = encode_tdp_limit_override(body_b, sizeof(body_b), 80);
	struct ccfgovr_bank_hdr hdr_b = {.magic = CCFGOVR_MAGIC, .seq = 3};

	write_bank(BANK_B_ADDR, &hdr_b, body_b, body_b_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), 80,
		      "expected fallback to bank B when equal-seq bank A has a bad CRC");
}

/**
 * @brief Test bad path when both tables are invalid
 */
ZTEST(bh_fwtable_ccfgovr, test_both_banks_invalid_no_override)
{
	uint8_t body[16];
	size_t body_len = encode_tdp_limit_override(body, sizeof(body), 140U);

	struct ccfgovr_bank_hdr hdr_a = {
		.magic = CCFGOVR_MAGIC,
		.seq = 3,
		.cksum = 0xAAAA5555U,
	};
	struct ccfgovr_bank_hdr hdr_b = {
		.magic = 0xBADBADBAU,
		.seq = 2,
	};

	write_bank(BANK_A_ADDR, &hdr_a, body, body_len);
	write_bank(BANK_B_ADDR, &hdr_b, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), DEFAULT_TDP_LIMIT,
		      "override must not be applied when both banks invalid");
}

/* ------------------------------------------------------------------------- */
/* Header validation                                                         */
/* ------------------------------------------------------------------------- */

/**
 * @brief A bank with the wrong magic must be ignored.
 */
ZTEST(bh_fwtable_ccfgovr, test_wrong_magic_rejected)
{
	uint8_t body[16];
	size_t body_len = encode_tdp_limit_override(body, sizeof(body), 111);
	struct ccfgovr_bank_hdr hdr = {.magic = 0xDEADBEEFU, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), DEFAULT_TDP_LIMIT,
		      "override must not be applied for a bank with the wrong magic");
}

/**
 * @brief A bank whose seq equals the erased sentinel must be ignored.
 */
ZTEST(bh_fwtable_ccfgovr, test_seq_erased_rejected)
{
	uint8_t body[16];
	size_t body_len = encode_tdp_limit_override(body, sizeof(body), 112);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = CCFGOVR_SEQ_ERASED};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), DEFAULT_TDP_LIMIT,
		      "override must not be applied for an erased-sentinel seq");
}

/**
 * @brief A bank with an unrecognised header version must be ignored.
 */
ZTEST(bh_fwtable_ccfgovr, test_wrong_version_rejected)
{
	uint8_t body[16];
	size_t body_len = encode_tdp_limit_override(body, sizeof(body), 113);
	struct ccfgovr_bank_hdr hdr = {
		.magic = CCFGOVR_MAGIC,
		.seq = 2,
		.version = CCFGOVR_HDR_VERSION + 1,
	};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), DEFAULT_TDP_LIMIT,
		      "override must not be applied for an unknown header version");
}

/**
 * @brief A body length that is not a multiple of 4 must be ignored.
 */
ZTEST(bh_fwtable_ccfgovr, test_body_len_unaligned_rejected)
{
	uint8_t body[16];

	memset(body, 0, sizeof(body));
	(void)encode_tdp_limit_override(body, sizeof(body), 114);

	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	/* Force a non-multiple-of-4 body length. */
	write_bank(BANK_A_ADDR, &hdr, body, 5);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), DEFAULT_TDP_LIMIT,
		      "override must not be applied for a misaligned body length");
}

/**
 * @brief A body length above the decode cap must be ignored.
 */
ZTEST(bh_fwtable_ccfgovr, test_body_len_too_large_rejected)
{
	static uint8_t body[CCFGOVR_DECODE_BODY_MAX + 8];

	memset(body, 0, sizeof(body));

	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, CCFGOVR_DECODE_BODY_MAX + 4U);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), DEFAULT_TDP_LIMIT,
		      "override must not be applied for an over-cap body length");
}

/**
 * @brief A body length exactly at the decode cap must still be accepted.
 */
ZTEST(bh_fwtable_ccfgovr, test_body_len_at_max_boundary_applies)
{
	static uint8_t body[CCFGOVR_DECODE_BODY_MAX];

	memset(body, 0, sizeof(body));
	/* Real override at the front; the trailing zeros act as protobuf
	 * null terminators and CRC padding.
	 */
	(void)encode_tdp_limit_override(body, sizeof(body), 165);

	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, CCFGOVR_DECODE_BODY_MAX);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), 165, "override at the maximum body length must be applied");
}

/**
 * @brief A boot-fs entry smaller than the header must be ignored.
 */
ZTEST(bh_fwtable_ccfgovr, test_image_size_smaller_than_header_rejected)
{
	/* Shrink bank A's descriptor so its image_size is below the header size. */
	setup_fds(true, sizeof(struct ccfgovr_bank_hdr) - 4U, true, BANK_SIZE);

	uint8_t body[16];
	size_t body_len = encode_tdp_limit_override(body, sizeof(body), 116);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), DEFAULT_TDP_LIMIT,
		      "override must not be applied when image_size is below the header size");
}

/**
 * @brief A missing bank descriptor must be skipped, and the other bank used.
 */
ZTEST(bh_fwtable_ccfgovr, test_missing_bank_tag_uses_other_bank)
{
	/* Only bank B has a descriptor. */
	setup_fds(false, 0, true, BANK_SIZE);

	uint8_t body[16];
	size_t body_len = encode_tdp_limit_override(body, sizeof(body), 133);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_B_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), 133,
		      "expected bank B to be used when bank A descriptor is missing");
}

/* ------------------------------------------------------------------------- */
/* Empty body                                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief A valid, empty (body_len==0) override is a no-op.
 */
ZTEST(bh_fwtable_ccfgovr, test_empty_body_is_noop)
{
	uint8_t dummy = 0;
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, &dummy, 0);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), DEFAULT_TDP_LIMIT,
		      "an empty override must leave the table unchanged");
}

/**
 * @brief A valid, empty newest bank terminates the search (older banks are not
 *        consulted).
 */
ZTEST(bh_fwtable_ccfgovr, test_empty_newest_bank_stops_search)
{
	uint8_t dummy = 0;
	struct ccfgovr_bank_hdr hdr_a = {.magic = CCFGOVR_MAGIC, .seq = 5};

	write_bank(BANK_A_ADDR, &hdr_a, &dummy, 0);

	uint8_t body_b[16];
	size_t body_b_len = encode_tdp_limit_override(body_b, sizeof(body_b), 120);
	struct ccfgovr_bank_hdr hdr_b = {.magic = CCFGOVR_MAGIC, .seq = 4};

	write_bank(BANK_B_ADDR, &hdr_b, body_b, body_b_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), DEFAULT_TDP_LIMIT,
		      "a valid empty newest bank must stop the search, not fall through");
}

/* ------------------------------------------------------------------------- */
/* Merge semantics and security                                              */
/* ------------------------------------------------------------------------- */

/**
 * @brief A reserved field (e.g. fw_bundle_version, field 1) must be ignored.
 *
 * Hand-encoded body carrying a forged top-level field 1 plus a valid
 * chip_limits.tdp_limit (field 2 -> field 5). The reserved field is unknown to
 * FwTableOverride_fields and must be silently skipped, while the allow-listed
 * tdp_limit is still applied and fw_bundle_version is left untouched.
 */
ZTEST(bh_fwtable_ccfgovr, test_reserved_field_is_ignored)
{
	static const uint8_t body[] = {
		0x08, 0xB9, 0x60,             /* field 1 (reserved) varint = 12345 */
		0x12, 0x03, 0x28, 0xAF, 0x01, /* field 2 chip_limits { field 5 tdp = 175 } */
		0x00,                         /* null terminator */
		0x00, 0x00, 0x00,             /* pad to 4-byte boundary */
	};

	FwTable *t = fw_table();

	t->fw_bundle_version = 0xABCD1234U;

	write_raw_bank(BANK_A_ADDR, 2, body, sizeof(body));

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);

	zassert_equal(tdp_limit(), 175, "allow-listed tdp_limit must still be applied");
	zassert_equal(t->fw_bundle_version, 0xABCD1234U,
		      "reserved fw_bundle_version must not be forgeable via ccfgovr");
}

/**
 * @brief An unknown (high-numbered) field must be skipped without failing decode.
 */
ZTEST(bh_fwtable_ccfgovr, test_unknown_field_is_skipped)
{
	static const uint8_t body[] = {
		0x90, 0x03, 0x01,             /* field 50 (unknown) varint = 1 */
		0x12, 0x03, 0x28, 0xA0, 0x01, /* field 2 chip_limits { field 5 tdp = 160 } */
		0x00,                         /* null terminator */
		0x00, 0x00, 0x00,             /* pad to 4-byte boundary */
	};

	write_raw_bank(BANK_A_ADDR, 2, body, sizeof(body));

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), 160,
		      "decode must skip unknown fields and still apply known ones");
}

/**
 * @brief Overriding only feature_enable must leave chip_limits fields intact.
 */
ZTEST(bh_fwtable_ccfgovr, test_partial_override_preserves_other_fields)
{
	uint8_t body[16];
	/* Only feature_enable.kernel_throttler_at_floor_en present. */
	size_t body_len = encode_override(body, sizeof(body), false, 0, false, 0, true, true);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);

	zassert_true(fw_table()->feature_enable.kernel_throttler_at_floor_en,
		     "feature_enable override should be applied");
	zassert_equal(tdp_limit(), DEFAULT_TDP_LIMIT,
		      "tdp_limit must be preserved when not present in the override");
	zassert_equal(fw_table()->chip_limits.kernel_throttler_stop_nops_freq, 0U,
		      "stop_nops_freq must be preserved when not present in the override");
}

/**
 * @brief chip_limits present but tdp_limit absent must preserve tdp_limit.
 */
ZTEST(bh_fwtable_ccfgovr, test_chip_limits_without_tdp_preserves_tdp)
{
	uint8_t body[16];
	/* chip_limits present, only kernel_throttler_stop_nops_freq set. */
	size_t body_len = encode_override(body, sizeof(body), false, 0, true, 777, false, false);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);

	zassert_equal(fw_table()->chip_limits.kernel_throttler_stop_nops_freq, 777U,
		      "stop_nops_freq override should be applied");
	zassert_equal(tdp_limit(), DEFAULT_TDP_LIMIT,
		      "tdp_limit must be preserved when its sub-field is absent");
}

/**
 * @brief An explicit value of 0 must be applied (distinct from "unset").
 */
ZTEST(bh_fwtable_ccfgovr, test_explicit_zero_value_applies)
{
	uint8_t body[16];
	size_t body_len = encode_tdp_limit_override(body, sizeof(body), 0);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tdp_limit(), 0U, "an explicit zero override must be applied");
}

/**
 * @brief All allow-listed fields can be applied together from a single body.
 */
ZTEST(bh_fwtable_ccfgovr, test_all_allowlisted_fields_apply_together)
{
	uint8_t body[24];
	size_t body_len = encode_override(body, sizeof(body), true, 130, true, 900, true, true);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);

	zassert_equal(tdp_limit(), 130, "tdp_limit should be applied");
	zassert_equal(fw_table()->chip_limits.kernel_throttler_stop_nops_freq, 900U,
		      "stop_nops_freq should be applied");
	zassert_true(fw_table()->feature_enable.kernel_throttler_at_floor_en,
		     "kernel_throttler_at_floor_en should be applied");
}

#else /* !CONFIG_BH_FWTABLE_CCFGOVR */

/**
 * @brief Build/run variant proving the feature compiles out cleanly.
 *
 * With CONFIG_BH_FWTABLE_CCFGOVR disabled, tt_bh_fwtable_apply_ccfgovr() is not
 * compiled into the driver, so the firmware table is used verbatim. This guards
 * the #ifdef plumbing and ensures the rest of the driver still builds.
 */
ZTEST_SUITE(bh_fwtable_ccfgovr_disabled, NULL, NULL, NULL, NULL, NULL);

ZTEST(bh_fwtable_ccfgovr_disabled, test_feature_compiled_out)
{
	zassert_false(IS_ENABLED(CONFIG_BH_FWTABLE_CCFGOVR),
		      "expected CCFGOVR to be disabled in this build variant");
}

#endif /* CONFIG_BH_FWTABLE_CCFGOVR */
