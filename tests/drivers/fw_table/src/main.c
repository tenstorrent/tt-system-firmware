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

#include <pb_common.h>
#include <pb_encode.h>
#include <tenstorrent/tt_boot_fs.h>

#include "fw_table_override.pb.h"

/* Mirror of drivers/misc/bh_fwtable/ccfgovr.h. */
#define CCFGOVR_MAGIC           0x564F4343U
#define CCFGOVR_HDR_VERSION     0U
#define CCFGOVR_SEQ_ERASED      0xFFFFFFFFU
#define CCFGOVR_DECODE_BODY_MAX 512U

#define CCFGOVR_STATE(a, b)                                                                        \
	(FIELD_PREP(BH_FWTABLE_CCFGOVR_STATE_BANK_A, BH_FWTABLE_CCFGOVR_BANK_##a) |                \
	 FIELD_PREP(BH_FWTABLE_CCFGOVR_STATE_BANK_B, BH_FWTABLE_CCFGOVR_BANK_##b))

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
/* Stands in for a cmfwcfg that pins the link speed, as the Galaxy tables do. */
#define DEFAULT_ETH_SPEED 200U
/* Distinct per instance so a pci0 override cannot silently rewrite pci1. */
#define DEFAULT_PCI0_SPEED 0U
#define DEFAULT_PCI1_SPEED 5U

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
	zassert_equal(rc, 0, "flash_write of fd slot %zu failed with %d", slot, rc);
}

static size_t encode_override(const FwTableOverride *ovr, uint8_t *out, size_t out_size)
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

static size_t encode_tdp_limit_override(uint8_t *out, size_t out_size, uint32_t value)
{
	FwTableOverride ovr = FwTableOverride_init_zero;

	ovr.has_chip_limits = true;
	ovr.chip_limits.has_tdp_limit = true;
	ovr.chip_limits.tdp_limit = value;

	return encode_override(&ovr, out, out_size);
}

static size_t encode_max_tdp_limit_override(uint8_t *out, size_t out_size, uint32_t value)
{
	FwTableOverride ovr = FwTableOverride_init_zero;

	ovr.has_chip_limits = true;
	ovr.chip_limits.has_max_tdp_limit = true;
	ovr.chip_limits.max_tdp_limit = value;

	return encode_override(&ovr, out, out_size);
}

static size_t encode_kernel_throttler_override(uint8_t *out, size_t out_size, bool enabled,
					       uint32_t stop_freq)
{
	FwTableOverride ovr = FwTableOverride_init_zero;

	ovr.has_feature_enable = true;
	ovr.feature_enable.has_kernel_throttler_at_floor_en = true;
	ovr.feature_enable.kernel_throttler_at_floor_en = enabled;

	ovr.has_chip_limits = true;
	ovr.chip_limits.has_kernel_throttler_stop_nops_freq = true;
	ovr.chip_limits.kernel_throttler_stop_nops_freq = stop_freq;

	return encode_override(&ovr, out, out_size);
}

static size_t encode_eth_speed_override(uint8_t *out, size_t out_size, uint32_t speed)
{
	FwTableOverride ovr = FwTableOverride_init_zero;

	ovr.has_eth_property_table = true;
	ovr.eth_property_table.has_eth_speed_override = true;
	ovr.eth_property_table.eth_speed_override = speed;

	return encode_override(&ovr, out, out_size);
}

static size_t encode_pci_speed_override(uint8_t *out, size_t out_size, uint8_t inst, uint32_t speed)
{
	FwTableOverride ovr = FwTableOverride_init_zero;

	switch (inst) {
	case 0U:
		ovr.has_pci0_property_table = true;
		ovr.pci0_property_table.has_max_pcie_speed = true;
		ovr.pci0_property_table.max_pcie_speed = speed;
		break;
	case 1U:
		ovr.has_pci1_property_table = true;
		ovr.pci1_property_table.has_max_pcie_speed = true;
		ovr.pci1_property_table.max_pcie_speed = speed;
		break;
	default:
		zassert_unreachable("invalid instance");
	}

	return encode_override(&ovr, out, out_size);
}

static size_t encode_gddr_therm_trip_override(uint8_t *out, size_t out_size, bool enabled)
{
	FwTableOverride ovr = FwTableOverride_init_zero;

	ovr.has_feature_enable = true;
	ovr.feature_enable.has_gddr_therm_trip_en = true;
	ovr.feature_enable.gddr_therm_trip_en = enabled;

	return encode_override(&ovr, out, out_size);
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
	t->eth_property_table.has_eth_speed_override = true;
	t->eth_property_table.eth_speed_override = DEFAULT_ETH_SPEED;
	t->pci0_property_table.max_pcie_speed = DEFAULT_PCI0_SPEED;
	t->pci1_property_table.max_pcie_speed = DEFAULT_PCI1_SPEED;
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
	zassert_equal(
		tt_bh_fwtable_get_fw_table(fwtable_dev)->eth_property_table.eth_speed_override,
		DEFAULT_ETH_SPEED, "fields absent from the override must keep their cmfwcfg value");
	zassert_equal(tt_bh_fwtable_get_ccfgovr_state(fwtable_dev),
		      CCFGOVR_STATE(APPLIED, EMPTY) | BH_FWTABLE_CCFGOVR_STATE_HAS_OVERRIDES);
	zassert_equal(tt_bh_fwtable_get_ccfgovr_seq(fwtable_dev), 2U);
}

/**
 * @brief Test that max_tdp_limit can be overridden independently of tdp_limit
 */
ZTEST(bh_fwtable_ccfgovr, test_max_tdp_limit_override_applies)
{
	uint8_t body[16];
	size_t body_len = encode_max_tdp_limit_override(body, sizeof(body), 250U);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.max_tdp_limit, 250U,
		      "expected override to set max_tdp_limit=250");
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit,
		      DEFAULT_TDP_LIMIT, "max_tdp_limit override must not clobber tdp_limit");
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
	zassert_true(tt_bh_fwtable_get_fw_table(fwtable_dev)
			     ->feature_enable.kernel_throttler_at_floor_en,
		     "expected override to enable kernel_throttler_at_floor_en");
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)
			      ->chip_limits.kernel_throttler_stop_nops_freq,
		      800U, "expected override to set kernel_throttler_stop_nops_freq=800");
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
	zassert_true(tt_bh_fwtable_get_fw_table(fwtable_dev)->feature_enable.gddr_therm_trip_en,
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
	zassert_equal(
		tt_bh_fwtable_get_fw_table(fwtable_dev)->eth_property_table.eth_speed_override,
		400U, "expected override to set eth_speed_override=400");
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
	zassert_true(
		tt_bh_fwtable_get_fw_table(fwtable_dev)->eth_property_table.has_eth_speed_override,
		"expected the override to set has_eth_speed_override");
	zassert_equal(
		tt_bh_fwtable_get_fw_table(fwtable_dev)->eth_property_table.eth_speed_override, 0U,
		"expected the override to set eth_speed_override=0 (auto)");
}

/**
 * @brief Test that pci0 max_pcie_speed can be capped at Gen 4
 *
 * pci1 must keep its cmfwcfg value: the two instances are independent.
 */
ZTEST(bh_fwtable_ccfgovr, test_pci0_max_pcie_speed_override_applies)
{
	uint8_t body[16];
	size_t body_len = encode_pci_speed_override(body, sizeof(body), 0U, 4U);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->pci0_property_table.max_pcie_speed,
		      4U, "expected override to set pci0 max_pcie_speed=4");
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->pci1_property_table.max_pcie_speed,
		      DEFAULT_PCI1_SPEED,
		      "pci0 override must not clobber pci1's cmfwcfg max_pcie_speed");
}

/**
 * @brief Test that pci1 max_pcie_speed can be set to Gen 5
 *
 * pci0 must keep its cmfwcfg value.
 */
ZTEST(bh_fwtable_ccfgovr, test_pci1_max_pcie_speed_override_applies)
{
	uint8_t body[16];
	size_t body_len = encode_pci_speed_override(body, sizeof(body), 1U, 5U);
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->pci1_property_table.max_pcie_speed,
		      5U, "expected override to set pci1 max_pcie_speed=5");
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->pci0_property_table.max_pcie_speed,
		      DEFAULT_PCI0_SPEED,
		      "pci1 override must not clobber pci0's cmfwcfg max_pcie_speed");
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
	zassert_equal(tt_bh_fwtable_get_ccfgovr_state(fwtable_dev),
		      CCFGOVR_STATE(STANDBY, APPLIED) | BH_FWTABLE_CCFGOVR_STATE_HAS_OVERRIDES);
	zassert_equal(tt_bh_fwtable_get_ccfgovr_seq(fwtable_dev), 2U);
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
	zassert_equal(tt_bh_fwtable_get_ccfgovr_state(fwtable_dev), CCFGOVR_STATE(REJECTED, EMPTY));
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
	zassert_equal(tt_bh_fwtable_get_ccfgovr_state(fwtable_dev),
		      CCFGOVR_STATE(REJECTED, APPLIED) | BH_FWTABLE_CCFGOVR_STATE_HAS_OVERRIDES);
	zassert_equal(tt_bh_fwtable_get_ccfgovr_seq(fwtable_dev), 4U);
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
	zassert_equal(tt_bh_fwtable_get_ccfgovr_state(fwtable_dev), CCFGOVR_STATE(REJECTED, EMPTY));
	zassert_equal(tt_bh_fwtable_get_ccfgovr_seq(fwtable_dev), 0U);
}

/**
 * @brief Test that a newer empty bank clears an older bank's override
 *
 * This is what `bh-mod res --all` writes.
 */
ZTEST(bh_fwtable_ccfgovr, test_newer_empty_bank_clears_older_override)
{
	uint8_t body_a[16];
	size_t body_a_len = encode_tdp_limit_override(body_a, sizeof(body_a), 77);
	struct ccfgovr_bank_hdr hdr_a = {.magic = CCFGOVR_MAGIC, .seq = 1};
	struct ccfgovr_bank_hdr hdr_b = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr_a, body_a, body_a_len);
	write_bank(BANK_B_ADDR, &hdr_b, NULL, 0);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit,
		      DEFAULT_TDP_LIMIT, "newer empty bank must clear the older bank's override");
	zassert_equal(tt_bh_fwtable_get_ccfgovr_state(fwtable_dev),
		      CCFGOVR_STATE(STANDBY, APPLIED));
	zassert_equal(tt_bh_fwtable_get_ccfgovr_seq(fwtable_dev), 2U);
}

/**
 * @brief Test that a corrupt newer empty bank resurrects the older override
 *
 * Clearing with `bh-mod res --all` does not erase the previous override; it
 * stays in the other bank and comes back if the empty bank is lost.
 */
ZTEST(bh_fwtable_ccfgovr, test_corrupt_empty_bank_resurrects_older_override)
{
	uint8_t body_a[16];
	size_t body_a_len = encode_tdp_limit_override(body_a, sizeof(body_a), 77);
	struct ccfgovr_bank_hdr hdr_a = {.magic = CCFGOVR_MAGIC, .seq = 1};
	struct ccfgovr_bank_hdr hdr_b = {.magic = CCFGOVR_MAGIC, .seq = 2, .cksum = 0x12345678U};

	write_bank(BANK_A_ADDR, &hdr_a, body_a, body_a_len);
	write_bank(BANK_B_ADDR, &hdr_b, NULL, 0);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit, 77U);
	zassert_equal(tt_bh_fwtable_get_ccfgovr_state(fwtable_dev),
		      CCFGOVR_STATE(APPLIED, REJECTED) | BH_FWTABLE_CCFGOVR_STATE_HAS_OVERRIDES);
	zassert_equal(tt_bh_fwtable_get_ccfgovr_seq(fwtable_dev), 1U);
}

/**
 * @brief Test that equal sequence numbers resolve to bank A
 */
ZTEST(bh_fwtable_ccfgovr, test_equal_seq_prefers_bank_a)
{
	uint8_t body_a[16];
	size_t body_a_len = encode_tdp_limit_override(body_a, sizeof(body_a), 77);
	struct ccfgovr_bank_hdr hdr_a = {.magic = CCFGOVR_MAGIC, .seq = 3};

	write_bank(BANK_A_ADDR, &hdr_a, body_a, body_a_len);

	uint8_t body_b[16];
	size_t body_b_len = encode_tdp_limit_override(body_b, sizeof(body_b), 88);
	struct ccfgovr_bank_hdr hdr_b = {.magic = CCFGOVR_MAGIC, .seq = 3};

	write_bank(BANK_B_ADDR, &hdr_b, body_b, body_b_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit, 77U);
	zassert_equal(tt_bh_fwtable_get_ccfgovr_state(fwtable_dev),
		      CCFGOVR_STATE(APPLIED, STANDBY) | BH_FWTABLE_CCFGOVR_STATE_HAS_OVERRIDES);
}

/**
 * @brief Test that sequence comparison survives wrap-around
 *
 * 0 follows 0xFFFFFFFE because 0xFFFFFFFF is reserved for an erased bank.
 */
ZTEST(bh_fwtable_ccfgovr, test_seq_wraparound_newer_wins)
{
	uint8_t body_a[16];
	size_t body_a_len = encode_tdp_limit_override(body_a, sizeof(body_a), 77);
	struct ccfgovr_bank_hdr hdr_a = {.magic = CCFGOVR_MAGIC, .seq = 0xFFFFFFFEU};

	write_bank(BANK_A_ADDR, &hdr_a, body_a, body_a_len);

	uint8_t body_b[16];
	size_t body_b_len = encode_tdp_limit_override(body_b, sizeof(body_b), 88);
	struct ccfgovr_bank_hdr hdr_b = {.magic = CCFGOVR_MAGIC, .seq = 0};

	write_bank(BANK_B_ADDR, &hdr_b, body_b, body_b_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit, 88U,
		      "seq 0 must be newer than seq 0xFFFFFFFE");
	zassert_equal(tt_bh_fwtable_get_ccfgovr_seq(fwtable_dev), 0U);
	zassert_equal(tt_bh_fwtable_get_ccfgovr_state(fwtable_dev),
		      CCFGOVR_STATE(STANDBY, APPLIED) | BH_FWTABLE_CCFGOVR_STATE_HAS_OVERRIDES);
}

/**
 * @brief Test that implausible headers are rejected before the body is read
 */
ZTEST(bh_fwtable_ccfgovr, test_implausible_headers_rejected)
{
	static uint8_t big_body[CCFGOVR_DECODE_BODY_MAX + 4U];
	uint8_t body[16];
	size_t body_len = encode_tdp_limit_override(body, sizeof(body), 140U);
	const struct {
		const char *what;
		struct ccfgovr_bank_hdr hdr;
		const uint8_t *body;
		size_t body_len;
	} cases[] = {
		{"wrong magic", {.magic = 0xBADBADBAU, .seq = 2}, body, body_len},
		{"erased seq", {.magic = CCFGOVR_MAGIC, .seq = CCFGOVR_SEQ_ERASED}, body, body_len},
		{"unknown version",
		 {.magic = CCFGOVR_MAGIC, .seq = 2, .version = CCFGOVR_HDR_VERSION + 1U},
		 body,
		 body_len},
		{"unaligned body_len", {.magic = CCFGOVR_MAGIC, .seq = 2}, body, body_len - 1U},
		{"oversized body_len",
		 {.magic = CCFGOVR_MAGIC, .seq = 2},
		 big_body,
		 sizeof(big_body)},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		struct ccfgovr_bank_hdr hdr = cases[i].hdr;

		erase_both_banks();
		reset_fwtable();
		write_bank(BANK_A_ADDR, &hdr, cases[i].body, cases[i].body_len);

		tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
		zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit,
			      DEFAULT_TDP_LIMIT, "%s: override must not be applied", cases[i].what);
		zassert_equal(tt_bh_fwtable_get_ccfgovr_state(fwtable_dev),
			      CCFGOVR_STATE(EMPTY, EMPTY), "%s: header must be rejected",
			      cases[i].what);
	}
}

/**
 * @brief Test that the dram_table soft-harvest mask can be overridden
 */
ZTEST(bh_fwtable_ccfgovr, test_soft_harvest_dram_mask_override_applies)
{
	FwTableOverride ovr = FwTableOverride_init_zero;
	uint8_t body[16];

	ovr.has_dram_table = true;
	ovr.dram_table.has_soft_harvest_dram_mask = true;
	ovr.dram_table.soft_harvest_dram_mask = BIT(3);

	size_t body_len = encode_override(&ovr, body, sizeof(body));
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_true(tt_bh_fwtable_get_fw_table(fwtable_dev)->has_dram_table,
		     "the merge must mark dram_table present so harvesting reads the mask");
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->dram_table.soft_harvest_dram_mask,
		      BIT(3));
}

/**
 * @brief Test that cable fault detection can be bypassed
 */
ZTEST(bh_fwtable_ccfgovr, test_bypass_cable_fault_override_applies)
{
	FwTableOverride ovr = FwTableOverride_init_zero;
	uint8_t body[16];

	ovr.has_feature_enable = true;
	ovr.feature_enable.has_bypass_cable_fault_detection = true;
	ovr.feature_enable.bypass_cable_fault_detection = true;

	size_t body_len = encode_override(&ovr, body, sizeof(body));
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_true(tt_bh_fwtable_get_fw_table(fwtable_dev)
			     ->feature_enable.bypass_cable_fault_detection);
}

/**
 * @brief Test that a reserved field cannot forge the bundle version
 *
 * FwTableOverride reserves field 1 (FwTable.fw_bundle_version). The decoder
 * must skip it and still apply the fields that follow.
 */
ZTEST(bh_fwtable_ccfgovr, test_reserved_bundle_version_field_ignored)
{
	/* fw_bundle_version=0x7F, then chip_limits { tdp_limit=175 }, then terminator */
	const uint8_t body[] = {0x08, 0x7F, 0x12, 0x03, 0x28, 0xAF, 0x01, 0x00};
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, sizeof(body));

	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->fw_bundle_version, 0U,
		      "override must not be able to set fw_bundle_version");
	zassert_equal(tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.tdp_limit, 175U);
}

static void set_every_leaf(FwTableOverride *ovr)
{
	pb_field_iter_t top;
	uint32_t value = 1000U;

	zassert_true(pb_field_iter_begin(&top, FwTableOverride_fields, ovr));
	do {
		pb_field_iter_t leaf;

		zassert_equal(PB_LTYPE(top.type), PB_LTYPE_SUBMESSAGE,
			      "FwTableOverride field %u is not a submessage", top.tag);
		*(bool *)top.pSize = true;
		zassert_true(pb_field_iter_begin(&leaf, top.submsg_desc, top.pData));
		do {
			*(bool *)leaf.pSize = true;
			switch (PB_LTYPE(leaf.type)) {
			case PB_LTYPE_BOOL:
				*(bool *)leaf.pData = true;
				break;
			case PB_LTYPE_UVARINT:
				zassert_equal(leaf.data_size, sizeof(uint32_t));
				*(uint32_t *)leaf.pData = value++;
				break;
			default:
				zassert_unreachable("field %u.%u has a type this test does not set",
						    top.tag, leaf.tag);
			}
		} while (pb_field_iter_next(&leaf));
	} while (pb_field_iter_next(&top));
}

/**
 * @brief Test that every field in fw_table_override.proto is merged
 *
 * Field numbers in FwTableOverride match FwTable, so each override leaf can
 * be found in FwTable by tag. A field added to the proto without a merge
 * branch in tt_bh_fwtable_apply_ccfgovr() fails here.
 */
ZTEST(bh_fwtable_ccfgovr, test_every_override_field_is_merged)
{
	FwTableOverride ovr = FwTableOverride_init_zero;
	uint8_t body[128];
	pb_field_iter_t top;

	set_every_leaf(&ovr);

	size_t body_len = encode_override(&ovr, body, sizeof(body));
	struct ccfgovr_bank_hdr hdr = {.magic = CCFGOVR_MAGIC, .seq = 2};

	write_bank(BANK_A_ADDR, &hdr, body, body_len);
	tt_bh_fwtable_apply_ccfgovr(fwtable_dev);

	const FwTable *t = tt_bh_fwtable_get_fw_table(fwtable_dev);

	zassert_true(pb_field_iter_begin(&top, FwTableOverride_fields, &ovr));
	do {
		pb_field_iter_t dst_top;
		pb_field_iter_t leaf;

		zassert_true(pb_field_iter_begin_const(&dst_top, FwTable_fields, t));
		zassert_true(pb_field_iter_find(&dst_top, top.tag), "FwTable has no field %u",
			     top.tag);
		zassert_true(pb_field_iter_begin(&leaf, top.submsg_desc, top.pData));
		do {
			pb_field_iter_t dst_leaf;

			zassert_true(pb_field_iter_begin_const(&dst_leaf, dst_top.submsg_desc,
							       dst_top.pData));
			zassert_true(pb_field_iter_find(&dst_leaf, leaf.tag),
				     "FwTable field %u has no field %u", top.tag, leaf.tag);
			zassert_equal(leaf.data_size, dst_leaf.data_size,
				      "field %u.%u has a different size in FwTable", top.tag,
				      leaf.tag);
			zassert_mem_equal(leaf.pData, dst_leaf.pData, leaf.data_size,
					  "field %u.%u was not merged into FwTable", top.tag,
					  leaf.tag);
		} while (pb_field_iter_next(&leaf));
	} while (pb_field_iter_next(&top));
}
