/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gddr_cal.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(gddr_cal, CONFIG_TT_APP_LOG_LEVEL);

#if PARTITION_EXISTS(gddrcal_a) && PARTITION_EXISTS(gddrcal_b)

/* "GCAL" little-endian */
#define GDDR_CAL_MAGIC      0x4C414347U
/*
 * Bump on any layout-changing edit to struct gddr_cal_bank_hdr, to
 * gddr_cal_table_t, or to the on-flash framing. Banks stamped with another
 * version fail validation and are treated as absent, so the flow re-latches.
 * v1 was per-controller only, v2 added the per-ASIC dimension, v3 added the A/B
 * bank framing.
 */
#define GDDR_CAL_VERSION    3U
#define GDDR_CAL_SEQ_ERASED 0xFFFFFFFFU

#define GDDR_CAL_NUM_BANKS 2

/*
 * On-flash bank framing, modelled on struct ccfgovr_bank_hdr. cksum is last so
 * that it covers every header field that precedes it, concatenated with the
 * table body.
 */
struct gddr_cal_bank_hdr {
	uint32_t magic;   /* must equal GDDR_CAL_MAGIC */
	uint32_t seq;     /* monotonic; GDDR_CAL_SEQ_ERASED is reserved */
	uint32_t version; /* must equal GDDR_CAL_VERSION */
	uint32_t cksum;   /* CRC32 (IEEE 802.3) over hdr[0 .. offsetof(cksum)) || table */
};

struct gddr_cal_bank {
	struct gddr_cal_bank_hdr hdr;
	gddr_cal_table_t table;
};

BUILD_ASSERT(DT_SAME_NODE(DT_MTD_FROM_FIXED_PARTITION(DT_NODELABEL(gddrcal_a)),
			  DT_MTD_FROM_FIXED_PARTITION(DT_NODELABEL(gddrcal_b))),
	     "gddrcal banks must live on the same flash device");
BUILD_ASSERT(PARTITION_SIZE(gddrcal_a) == PARTITION_SIZE(gddrcal_b),
	     "gddrcal banks must be the same size");
BUILD_ASSERT(sizeof(struct gddr_cal_bank) <= PARTITION_SIZE(gddrcal_a),
	     "gddr_cal_table_t does not fit in a gddrcal bank");

#define GDDRCAL_BANK_SIZE PARTITION_SIZE(gddrcal_a)

static const struct device *const flash = PARTITION_DEVICE(gddrcal_a);
static const uint32_t bank_offset[GDDR_CAL_NUM_BANKS] = {
	PARTITION_OFFSET(gddrcal_a),
	PARTITION_OFFSET(gddrcal_b),
};

/*
 * Which bank currently holds the live table, and its sequence number. Tracked so
 * that gddr_cal_write() knows which bank is safe to erase. Cleared by a failed
 * read, in which case the next write starts over at bank A with seq 0.
 */
static uint32_t active_seq;
static uint8_t active_bank;
static bool have_active;

/*
 * Scratch buffer for the bank being read or written. A file static rather than a
 * stack local because struct gddr_cal_bank is a few hundred bytes and the main
 * thread stack is 1 KiB by default; gddr_cal_read() and gddr_cal_write() are only
 * reachable from single-threaded init and from msgqueue handling, never
 * concurrently.
 */
static struct gddr_cal_bank bank_buf;

static bool gddr_cal_hdr_is_plausible(const struct gddr_cal_bank_hdr *hdr)
{
	return hdr->magic == GDDR_CAL_MAGIC && hdr->seq != GDDR_CAL_SEQ_ERASED &&
	       hdr->version == GDDR_CAL_VERSION;
}

/* Wrap-safe "is a newer than b". */
static bool gddr_cal_seq_is_newer(uint32_t a, uint32_t b)
{
	return ((int32_t)(a - b)) > 0;
}

static uint32_t gddr_cal_bank_crc(const struct gddr_cal_bank *bank)
{
	uint32_t crc = crc32_ieee_update(0, (const uint8_t *)&bank->hdr,
					 offsetof(struct gddr_cal_bank_hdr, cksum));

	return crc32_ieee_update(crc, (const uint8_t *)&bank->table, sizeof(bank->table));
}

int gddr_cal_read(gddr_cal_table_t *table)
{
	struct gddr_cal_bank_hdr hdr[GDDR_CAL_NUM_BANKS];
	uint8_t order[GDDR_CAL_NUM_BANKS];
	uint8_t n_candidates = 0;
	int rc;

	have_active = false;

	if (!device_is_ready(flash)) {
		return -ENODEV;
	}

	/* Read both headers and build an ordered candidate list, newest first. */
	for (uint8_t i = 0; i < GDDR_CAL_NUM_BANKS; i++) {
		rc = flash_read(flash, bank_offset[i], &hdr[i], sizeof(hdr[i]));
		if (rc < 0) {
			LOG_ERR("%s() failed: %d", "flash_read", rc);
			continue;
		}

		if (gddr_cal_hdr_is_plausible(&hdr[i])) {
			order[n_candidates++] = i;
		}
	}

	if (n_candidates == GDDR_CAL_NUM_BANKS &&
	    gddr_cal_seq_is_newer(hdr[order[1]].seq, hdr[order[0]].seq)) {
		uint8_t tmp = order[0];

		order[0] = order[1];
		order[1] = tmp;
	}

	/* Try candidates in order; a torn newest bank falls back to the older one. */
	for (uint8_t i = 0; i < n_candidates; i++) {
		uint8_t b = order[i];

		rc = flash_read(flash, bank_offset[b], &bank_buf, sizeof(bank_buf));
		if (rc < 0) {
			LOG_ERR("%s() failed: %d", "flash_read", rc);
			continue;
		}

		if (bank_buf.hdr.cksum != gddr_cal_bank_crc(&bank_buf)) {
			LOG_WRN("gddrcal bank %u CRC mismatch, ignoring", b);
			continue;
		}

		*table = bank_buf.table;
		active_bank = b;
		active_seq = bank_buf.hdr.seq;
		have_active = true;
		LOG_DBG("Using gddrcal bank %u seq %u", b, bank_buf.hdr.seq);
		return 0;
	}

	return -ENOENT;
}

int gddr_cal_write(const gddr_cal_table_t *table)
{
	uint32_t seq;
	uint8_t target;
	int rc;

	if (!device_is_ready(flash)) {
		return -ENODEV;
	}

	/*
	 * Always target the bank that is not currently active, so the live table
	 * is never erased and a power loss at any point below leaves it readable.
	 */
	target = have_active ? (active_bank ^ 1) : 0;
	seq = have_active ? active_seq + 1 : 0;
	if (seq == GDDR_CAL_SEQ_ERASED) {
		/* Reserved as the erased value; skip it. The wrap-safe sequence
		 * comparison still orders 0 as newer than 0xfffffffe.
		 */
		seq = 0;
	}

	bank_buf.hdr.magic = GDDR_CAL_MAGIC;
	bank_buf.hdr.seq = seq;
	bank_buf.hdr.version = GDDR_CAL_VERSION;
	bank_buf.table = *table;
	bank_buf.hdr.cksum = gddr_cal_bank_crc(&bank_buf);

	rc = flash_erase(flash, bank_offset[target], GDDRCAL_BANK_SIZE);
	if (rc < 0) {
		LOG_ERR("%s() failed: %d", "flash_erase", rc);
		return rc;
	}

	/*
	 * Body first, header last: until the header lands the bank fails the magic
	 * check, so a torn write can never be mistaken for a complete record.
	 */
	rc = flash_write(flash, bank_offset[target] + offsetof(struct gddr_cal_bank, table),
			 &bank_buf.table, sizeof(bank_buf.table));
	if (rc < 0) {
		LOG_ERR("%s() failed: %d", "flash_write", rc);
		return rc;
	}

	rc = flash_write(flash, bank_offset[target], &bank_buf.hdr, sizeof(bank_buf.hdr));
	if (rc < 0) {
		LOG_ERR("%s() failed: %d", "flash_write", rc);
		return rc;
	}

	/* Read the bank back and re-verify it. The CRC covers the body, so this
	 * needs no second copy of the table to compare against.
	 */
	rc = flash_read(flash, bank_offset[target], &bank_buf, sizeof(bank_buf));
	if (rc < 0) {
		LOG_ERR("%s() failed: %d", "flash_read", rc);
		return rc;
	}

	if (!gddr_cal_hdr_is_plausible(&bank_buf.hdr) || bank_buf.hdr.seq != seq ||
	    bank_buf.hdr.cksum != gddr_cal_bank_crc(&bank_buf)) {
		LOG_ERR("gddrcal bank %u write verification failed", target);
		return -EIO;
	}

	active_bank = target;
	active_seq = seq;
	have_active = true;
	LOG_DBG("Wrote gddrcal bank %u seq %u", target, seq);
	return 0;
}

#else /* !(PARTITION_EXISTS(gddrcal_a) && PARTITION_EXISTS(gddrcal_b)) */

int gddr_cal_read(gddr_cal_table_t *table)
{
	ARG_UNUSED(table);
	return -ENOSYS;
}

int gddr_cal_write(const gddr_cal_table_t *table)
{
	ARG_UNUSED(table);
	return -ENOSYS;
}

#endif /* PARTITION_EXISTS(gddrcal_a) && PARTITION_EXISTS(gddrcal_b) */
