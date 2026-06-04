/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
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

#include <pb_encode.h>
#include <tenstorrent/tt_boot_fs.h>

#include "fw_table_override.pb.h"

/* Mirror of drivers/misc/bh_fwtable/ccfgovr.h. */
#define CCFGOVR_MAGIC       0x564F4343U
#define CCFGOVR_HDR_VERSION 0U

struct ccfgovr_bank_hdr {
	uint32_t magic;
	uint32_t seq;
	uint32_t body_len;
	uint32_t version;
	uint32_t cksum;
};

#define FLASH_NODE   DT_NODELABEL(flashcontroller0)
#define FWTABLE_NODE DT_NODELABEL(fwtable)

static const struct device *const flash_dev = DEVICE_DT_GET(FLASH_NODE);
static const struct device *const fwtable_dev = DEVICE_DT_GET(FWTABLE_NODE);

#define BANK_A_ADDR 0x00010000U
#define BANK_B_ADDR 0x00011000U
#define BANK_SIZE   0x00001000U

#define FD_AREA_ERASE_SIZE 0x1000U

#define DEFAULT_TDP_LIMIT 150U

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
	zassert_equal(rc, 0, "flash_write failed with %d", slot, rc);
}

static size_t encode_tdp_limit_override(uint8_t *out, size_t out_size, uint32_t value)
{
	FwTableOverride ovr = FwTableOverride_init_zero;

	ovr.has_chip_limits = true;
	ovr.chip_limits.has_tdp_limit = true;
	ovr.chip_limits.tdp_limit = value;

	pb_ostream_t stream = pb_ostream_from_buffer(out, out_size);

	zassert_true(pb_encode_ex(&stream, FwTableOverride_fields, &ovr, PB_ENCODE_NULLTERMINATED),
		     "pb_encode_ex failed: %s", PB_GET_ERROR(&stream));

	size_t total = stream.bytes_written;

	/* Pad to 4 byte alignment for ccfgovr header requirement */
	while ((total % 4U) != 0U) {
		zassert_true(total < out_size, "padded body overruns buffer");
		out[total++] = 0x00U;
	}
	return total;
}

static void write_bank(uint32_t addr, struct ccfgovr_bank_hdr *hdr, const uint8_t *body,
		       size_t body_len)
{
	int rc = flash_erase(flash_dev, addr, BANK_SIZE);

	zassert_equal(rc, 0, "flash_erase bank@0x%x failed with %d", addr, rc);

	hdr->body_len = (uint32_t)body_len;
	if (hdr->cksum == 0) {
		uint32_t crc = crc32_ieee_update(0, (const uint8_t *)hdr,
						 offsetof(struct ccfgovr_bank_hdr, cksum));

		crc = crc32_ieee_update(crc, body, body_len);
		hdr->cksum = crc;
	}

	rc = flash_write(flash_dev, addr, hdr, sizeof(*hdr));
	zassert_equal(rc, 0, "flash_write hdr@0x%x failed with %d", addr, rc);

	if (body_len > 0) {
		rc = flash_write(flash_dev, addr + (uint32_t)sizeof(*hdr), body, body_len);
		zassert_equal(rc, 0, "flash_write body@0x%x failed with %d",
			      addr + (uint32_t)sizeof(*hdr), rc);
	}
}

static void erase_both_banks(void)
{
	int rc;

	rc = flash_erase(flash_dev, BANK_A_ADDR, BANK_SIZE);
	zassert_equal(rc, 0, "flash_erase BANK_A failed with %d", rc);

	rc = flash_erase(flash_dev, BANK_B_ADDR, BANK_SIZE);
	zassert_equal(rc, 0, "flash_erase BANK_B failed with %d", rc);
}

static void reset_fwtable(void)
{
	FwTable *t = (FwTable *)tt_bh_fwtable_get_fw_table(fwtable_dev);

	memset(t, 0, sizeof(*t));
	t->chip_limits.tdp_limit = DEFAULT_TDP_LIMIT;
}

/**
 * @brief Setup minimal boot_fs with ccfgovr a/b as the only entries
 */
static void *suite_setup(void)
{
	int rc;

	zassert_true(device_is_ready(flash_dev));

	rc = flash_erase(flash_dev, TT_BOOT_FS_FD_HEAD_ADDR, FD_AREA_ERASE_SIZE);
	zassert_equal(rc, 0, "flash_erase failed with %d", rc);

	write_fd(0, "ccfgovra", BANK_A_ADDR, BANK_SIZE);
	write_fd(1, "ccfgovrb", BANK_B_ADDR, BANK_SIZE);

	return NULL;
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);

	erase_both_banks();
	reset_fwtable();
}

ZTEST_SUITE(bh_fwtable_ccfgovr, NULL, suite_setup, before_each, NULL, NULL);

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
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit, 175,
		      "expected override to set tdp_limit=175");
}

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
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit, 88,
		      "expected bank B's newer-seq override to win (tdp_limit=88, not 77)");
}

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
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit,
		      DEFAULT_TDP_LIMIT, "override must not be applied when protobuf decode fails");
}

/**
 * @brief Test bad path when protobuf decode fails on the body
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

	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit,
		      DEFAULT_TDP_LIMIT, "override must not be applied when body CRC mismatches");
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
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit, 120,
		      "expected fallback to inactive bank's override (tdp_limit=120)");
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
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit,
		      DEFAULT_TDP_LIMIT, "override must not be applied when both banks invalid");
}
