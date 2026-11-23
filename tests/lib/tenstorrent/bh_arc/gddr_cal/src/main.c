/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gddr_cal.h"

/* Mirror of the private on-flash framing in lib/tenstorrent/bh_arc/gddr_cal.c. */
#define GDDR_CAL_MAGIC      0x4C414347U
#define GDDR_CAL_VERSION    4U
#define GDDR_CAL_VERSION_V3 3U
#define GDDR_CAL_SEQ_ERASED 0xFFFFFFFFU

struct gddr_cal_bank_hdr {
	uint32_t magic;
	uint32_t seq;
	uint32_t version;
	uint32_t cksum;
};

struct gddr_cal_bank {
	struct gddr_cal_bank_hdr hdr;
	gddr_cal_table_t table;
};

struct gddr_cal_entry_v3 {
	uint8_t ca_vrefc_offset;
	uint8_t ca_termination_offset;
	uint8_t ca_ocd_pulldown_offset;
	uint8_t valid;
};

struct gddr_cal_bank_v3 {
	struct gddr_cal_bank_hdr hdr;
	struct gddr_cal_entry_v3 entries[GDDR_CAL_NUM_ASICS][GDDR_CAL_NUM_CONTROLLERS];
};

#define NUM_BANKS 2

static const struct device *const flash_dev = PARTITION_DEVICE(gddrcal_a);
static const uint32_t bank_addr[NUM_BANKS] = {
	PARTITION_OFFSET(gddrcal_a),
	PARTITION_OFFSET(gddrcal_b),
};
#define BANK_SIZE PARTITION_SIZE(gddrcal_a)

static uint32_t bank_crc(const struct gddr_cal_bank *bank)
{
	uint32_t crc = crc32_ieee_update(0, (const uint8_t *)&bank->hdr,
					 offsetof(struct gddr_cal_bank_hdr, cksum));

	return crc32_ieee_update(crc, (const uint8_t *)&bank->table, sizeof(bank->table));
}

/* Distinct, deterministic payload per seed, so tests can tell tables apart. */
static void fill_table(gddr_cal_table_t *table, uint8_t seed)
{
	memset(table, 0, sizeof(*table));

	for (uint8_t asic = 0; asic < GDDR_CAL_NUM_ASICS; asic++) {
		for (uint8_t ctrl = 0; ctrl < GDDR_CAL_NUM_CONTROLLERS; ctrl++) {
			gddr_cal_entry_t *e = &table->entries[asic][ctrl];

			e->ca_vrefc_offset = (uint8_t)(seed + asic);
			e->ca_termination_offset = (uint8_t)(seed + ctrl);
			e->ca_phy_drv_tx_ca = (uint8_t)(seed ^ (asic * 8 + ctrl));
			e->valid = 1;
			e->ca_phase_offset[0] = (int8_t)(seed - ctrl);
			e->ca_phase_offset[1] = (int8_t)(ctrl - asic);
			e->ca_vrefc_offset_ch1 = (uint8_t)(seed + asic + ctrl);
		}
	}
}

static void erase_bank(uint8_t bank)
{
	int rc = flash_erase(flash_dev, bank_addr[bank], BANK_SIZE);

	zassert_ok(rc, "flash_erase bank %u failed: %d", bank, rc);
}

static void read_raw_bank(uint8_t bank, struct gddr_cal_bank *out)
{
	int rc = flash_read(flash_dev, bank_addr[bank], out, sizeof(*out));

	zassert_ok(rc, "flash_read bank %u failed: %d", bank, rc);
}

/*
 * Hand-place a bank on flash. When fix_crc is false the checksum is left
 * deliberately wrong, to stand in for a corrupted or torn record.
 */
static void write_raw_bank(uint8_t bank, uint32_t seq, uint32_t version,
			   const gddr_cal_table_t *table, bool fix_crc)
{
	struct gddr_cal_bank b;
	int rc;

	b.hdr.magic = GDDR_CAL_MAGIC;
	b.hdr.seq = seq;
	b.hdr.version = version;
	b.table = *table;
	b.hdr.cksum = fix_crc ? bank_crc(&b) : bank_crc(&b) ^ 0xffffffffU;

	erase_bank(bank);
	rc = flash_write(flash_dev, bank_addr[bank], &b, sizeof(b));
	zassert_ok(rc, "flash_write bank %u failed: %d", bank, rc);
}

/* A v3 bank as the previous firmware wrote it, with a good CRC. */
static void write_raw_v3_bank(uint8_t bank, uint32_t seq, uint8_t seed)
{
	struct gddr_cal_bank_v3 b;
	int rc;

	for (uint8_t asic = 0; asic < GDDR_CAL_NUM_ASICS; asic++) {
		for (uint8_t ctrl = 0; ctrl < GDDR_CAL_NUM_CONTROLLERS; ctrl++) {
			b.entries[asic][ctrl] = (struct gddr_cal_entry_v3){
				.ca_vrefc_offset = (uint8_t)(seed + asic),
				.ca_termination_offset = (uint8_t)(seed + ctrl),
				.ca_ocd_pulldown_offset = (uint8_t)(seed ^ (asic * 8 + ctrl)),
				.valid = (uint8_t)((asic + ctrl) & 1),
			};
		}
	}
	b.hdr.magic = GDDR_CAL_MAGIC;
	b.hdr.seq = seq;
	b.hdr.version = GDDR_CAL_VERSION_V3;
	b.hdr.cksum =
		crc32_ieee_update(crc32_ieee_update(0, (const uint8_t *)&b.hdr,
						    offsetof(struct gddr_cal_bank_hdr, cksum)),
				  (const uint8_t *)&b.entries, sizeof(b.entries));

	erase_bank(bank);
	rc = flash_write(flash_dev, bank_addr[bank], &b, sizeof(b));
	zassert_ok(rc, "flash_write bank %u failed: %d", bank, rc);
}

static bool raw_bank_is_valid(uint8_t bank, uint32_t *seq_out)
{
	struct gddr_cal_bank b;

	read_raw_bank(bank, &b);

	if (b.hdr.magic != GDDR_CAL_MAGIC || b.hdr.seq == GDDR_CAL_SEQ_ERASED ||
	    b.hdr.version != GDDR_CAL_VERSION || b.hdr.cksum != bank_crc(&b)) {
		return false;
	}

	if (seq_out != NULL) {
		*seq_out = b.hdr.seq;
	}

	return true;
}

/*
 * Erase both banks and drive a read, which both confirms the empty state and
 * resets gddr_cal.c's cached notion of which bank is active.
 */
static void before_each(void *fixture)
{
	gddr_cal_table_t table;

	ARG_UNUSED(fixture);

	zassert_true(device_is_ready(flash_dev), "flash device not ready");

	for (uint8_t i = 0; i < NUM_BANKS; i++) {
		erase_bank(i);
	}

	zassert_equal(gddr_cal_read(&table), -ENOENT, "expected -ENOENT after erasing both banks");
}

ZTEST_SUITE(gddr_cal, NULL, NULL, before_each, NULL, NULL);

ZTEST(gddr_cal, test_both_banks_erased_no_table)
{
	gddr_cal_table_t table;

	/* An erased bank is all 0xff, which fails the magic check. */
	zassert_equal(gddr_cal_read(&table), -ENOENT);
}

ZTEST(gddr_cal, test_write_then_read_round_trip)
{
	gddr_cal_table_t written;
	gddr_cal_table_t read_back;

	fill_table(&written, 0x11);
	zassert_ok(gddr_cal_write(&written));

	memset(&read_back, 0, sizeof(read_back));
	zassert_ok(gddr_cal_read(&read_back));
	zassert_mem_equal(&read_back, &written, sizeof(written),
			  "table did not survive the round trip");
}

ZTEST(gddr_cal, test_first_write_lands_in_bank_a)
{
	gddr_cal_table_t table;
	uint32_t seq;

	fill_table(&table, 0x22);
	zassert_ok(gddr_cal_write(&table));

	zassert_true(raw_bank_is_valid(0, &seq), "bank A should hold the first write");
	zassert_equal(seq, 0, "first write should use seq 0");
	zassert_false(raw_bank_is_valid(1, NULL), "bank B should still be erased");
}

ZTEST(gddr_cal, test_writes_alternate_banks_and_bump_seq)
{
	gddr_cal_table_t table;
	uint32_t seq_a;
	uint32_t seq_b;

	/* Three writes: A(seq 0), B(seq 1), A(seq 2). */
	for (uint8_t i = 0; i < 3; i++) {
		fill_table(&table, (uint8_t)(0x30 + i));
		zassert_ok(gddr_cal_write(&table), "write %u failed", i);
	}

	zassert_true(raw_bank_is_valid(0, &seq_a), "bank A invalid after 3 writes");
	zassert_true(raw_bank_is_valid(1, &seq_b), "bank B invalid after 3 writes");
	zassert_equal(seq_a, 2, "bank A should hold the newest write");
	zassert_equal(seq_b, 1, "bank B should hold the previous write");

	/* And the newest payload is what comes back. */
	gddr_cal_table_t expect;
	gddr_cal_table_t got;

	fill_table(&expect, 0x32);
	zassert_ok(gddr_cal_read(&got));
	zassert_mem_equal(&got, &expect, sizeof(expect), "read did not return the newest table");
}

/*
 * An update must never put the live table at risk. After a second write, the
 * bank holding the first write is still a complete, independently valid record.
 */
ZTEST(gddr_cal, test_active_bank_is_never_erased)
{
	gddr_cal_table_t first;
	gddr_cal_table_t second;
	uint32_t seq_a;
	uint32_t seq_b;

	fill_table(&first, 0x40);
	zassert_ok(gddr_cal_write(&first));
	zassert_true(raw_bank_is_valid(0, &seq_a));

	fill_table(&second, 0x50);
	zassert_ok(gddr_cal_write(&second));

	/* Both banks are now self-consistent records with different sequences. */
	zassert_true(raw_bank_is_valid(0, &seq_a), "previous bank was damaged by the update");
	zassert_true(raw_bank_is_valid(1, &seq_b), "new bank is not valid");
	zassert_not_equal(seq_a, seq_b, "banks must not share a sequence number");
}

ZTEST(gddr_cal, test_newer_seq_wins)
{
	gddr_cal_table_t older;
	gddr_cal_table_t newer;
	gddr_cal_table_t got;

	fill_table(&older, 0x60);
	fill_table(&newer, 0x70);

	/* Newest in bank B. */
	write_raw_bank(0, 5, GDDR_CAL_VERSION, &older, true);
	write_raw_bank(1, 6, GDDR_CAL_VERSION, &newer, true);
	zassert_ok(gddr_cal_read(&got));
	zassert_mem_equal(&got, &newer, sizeof(newer), "bank B (seq 6) should win");

	/* Newest in bank A. */
	write_raw_bank(0, 9, GDDR_CAL_VERSION, &newer, true);
	write_raw_bank(1, 8, GDDR_CAL_VERSION, &older, true);
	zassert_ok(gddr_cal_read(&got));
	zassert_mem_equal(&got, &newer, sizeof(newer), "bank A (seq 9) should win");
}

ZTEST(gddr_cal, test_seq_comparison_is_wrap_safe)
{
	gddr_cal_table_t older;
	gddr_cal_table_t newer;
	gddr_cal_table_t got;

	fill_table(&older, 0x80);
	fill_table(&newer, 0x90);

	/* 0 is one past 0xfffffffe once the reserved 0xffffffff is skipped. */
	write_raw_bank(0, 0xfffffffeU, GDDR_CAL_VERSION, &older, true);
	write_raw_bank(1, 0, GDDR_CAL_VERSION, &newer, true);

	zassert_ok(gddr_cal_read(&got));
	zassert_mem_equal(&got, &newer, sizeof(newer), "seq 0 should be newer than 0xfffffffe");
}

ZTEST(gddr_cal, test_corrupt_newest_falls_back_to_older)
{
	gddr_cal_table_t good;
	gddr_cal_table_t bad;
	gddr_cal_table_t got;

	fill_table(&good, 0xa0);
	fill_table(&bad, 0xb0);

	/* Bank B is newer but its checksum does not match. */
	write_raw_bank(0, 1, GDDR_CAL_VERSION, &good, true);
	write_raw_bank(1, 2, GDDR_CAL_VERSION, &bad, false);

	zassert_ok(gddr_cal_read(&got));
	zassert_mem_equal(&got, &good, sizeof(good), "should have fallen back to bank A");
}

/*
 * Simulate losing power partway through an update: the target bank has been
 * erased and its body written, but the header never landed.
 */
ZTEST(gddr_cal, test_interrupted_write_falls_back_to_older)
{
	gddr_cal_table_t good;
	gddr_cal_table_t partial;
	gddr_cal_table_t got;
	int rc;

	fill_table(&good, 0xc0);
	fill_table(&partial, 0xd0);

	write_raw_bank(0, 1, GDDR_CAL_VERSION, &good, true);

	erase_bank(1);
	rc = flash_write(flash_dev, bank_addr[1] + offsetof(struct gddr_cal_bank, table), &partial,
			 sizeof(partial));
	zassert_ok(rc, "flash_write body failed: %d", rc);

	/* Bank B's header is still erased, so it fails the magic check outright. */
	zassert_ok(gddr_cal_read(&got));
	zassert_mem_equal(&got, &good, sizeof(good), "torn bank must not be used");
}

ZTEST(gddr_cal, test_erased_seq_is_rejected)
{
	gddr_cal_table_t table;
	gddr_cal_table_t got;

	fill_table(&table, 0xe0);

	/* A bank whose seq reads as erased must never win, even with a good CRC. */
	write_raw_bank(0, GDDR_CAL_SEQ_ERASED, GDDR_CAL_VERSION, &table, true);
	zassert_equal(gddr_cal_read(&got), -ENOENT, "seq 0xffffffff must be rejected");
}

ZTEST(gddr_cal, test_wrong_version_is_rejected)
{
	gddr_cal_table_t table;
	gddr_cal_table_t got;

	fill_table(&table, 0xf0);

	/* Stands in for tables written by a much older and a newer firmware. */
	write_raw_bank(0, 3, GDDR_CAL_VERSION_V3 - 1, &table, true);
	write_raw_bank(1, 4, GDDR_CAL_VERSION + 1, &table, true);

	zassert_equal(gddr_cal_read(&got), -ENOENT, "foreign versions must be rejected");
}

ZTEST(gddr_cal, test_v3_bank_is_converted)
{
	gddr_cal_table_t got;

	write_raw_v3_bank(0, 5, 0x21);
	zassert_ok(gddr_cal_read(&got), "a v3 bank must still be readable");

	for (uint8_t asic = 0; asic < GDDR_CAL_NUM_ASICS; asic++) {
		for (uint8_t ctrl = 0; ctrl < GDDR_CAL_NUM_CONTROLLERS; ctrl++) {
			const gddr_cal_entry_t *e = &got.entries[asic][ctrl];

			zassert_equal(e->ca_vrefc_offset, (uint8_t)(0x21 + asic));
			zassert_equal(e->ca_termination_offset, (uint8_t)(0x21 + ctrl));
			/* The v3 byte was the DRAM OCD, never a drive code. */
			zassert_equal(e->ca_phy_drv_tx_ca, GDDR_CA_PHY_DRV_TX_CA_UNSET);
			zassert_equal(e->valid, (asic + ctrl) & 1);
			zassert_equal(e->ca_phase_offset[0], GDDR_CA_PHASE_OFFSET_UNSET);
			zassert_equal(e->ca_phase_offset[1], GDDR_CA_PHASE_OFFSET_UNSET);
			zassert_equal(e->ca_vrefc_offset_ch1, GDDR_CA_VREFC_OFFSET_CH1_SAME);
			zassert_equal(e->reserved, 0);
		}
	}
}

/* The first write after reading a v3 bank stores v4 in the other bank and leaves the v3 one. */
ZTEST(gddr_cal, test_write_after_v3_read_upgrades)
{
	gddr_cal_table_t table;
	gddr_cal_table_t got;
	uint32_t seq;
	struct gddr_cal_bank_v3 old;

	write_raw_v3_bank(0, 5, 0x21);
	zassert_ok(gddr_cal_read(&got));

	fill_table(&table, 0x44);
	zassert_ok(gddr_cal_write(&table));
	zassert_true(raw_bank_is_valid(1, &seq), "v4 write should target bank B");
	zassert_equal(seq, 6, "v4 write should follow the v3 sequence number");

	zassert_ok(flash_read(flash_dev, bank_addr[0], &old, sizeof(old)));
	zassert_equal(old.hdr.version, GDDR_CAL_VERSION_V3, "v3 bank must not be touched");

	zassert_ok(gddr_cal_read(&got));
	zassert_mem_equal(&got, &table, sizeof(table), "newer v4 bank should win");
}

ZTEST(gddr_cal, test_newer_v4_wins_over_v3)
{
	gddr_cal_table_t table;
	gddr_cal_table_t got;

	fill_table(&table, 0x55);
	write_raw_bank(0, 7, GDDR_CAL_VERSION, &table, true);
	write_raw_v3_bank(1, 6, 0x21);

	zassert_ok(gddr_cal_read(&got));
	zassert_mem_equal(&got, &table, sizeof(table), "v4 bank (seq 7) should win");
}

ZTEST(gddr_cal, test_both_banks_corrupt_no_table)
{
	gddr_cal_table_t table;
	gddr_cal_table_t got;

	fill_table(&table, 0x5a);

	write_raw_bank(0, 1, GDDR_CAL_VERSION, &table, false);
	write_raw_bank(1, 2, GDDR_CAL_VERSION, &table, false);

	zassert_equal(gddr_cal_read(&got), -ENOENT);
}

/* A failed read must not leave stale bank state behind: the next write starts over. */
ZTEST(gddr_cal, test_write_after_failed_read_starts_at_bank_a)
{
	gddr_cal_table_t table;
	gddr_cal_table_t got;
	uint32_t seq;

	fill_table(&table, 0x3c);

	write_raw_bank(0, 7, GDDR_CAL_VERSION, &table, false);
	write_raw_bank(1, 8, GDDR_CAL_VERSION, &table, false);
	zassert_equal(gddr_cal_read(&got), -ENOENT);

	zassert_ok(gddr_cal_write(&table));
	zassert_true(raw_bank_is_valid(0, &seq), "write should have targeted bank A");
	zassert_equal(seq, 0, "write after a failed read should restart at seq 0");
}
