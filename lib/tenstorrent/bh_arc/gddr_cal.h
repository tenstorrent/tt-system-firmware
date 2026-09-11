/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GDDR_CAL_H
#define GDDR_CAL_H

#include <stdint.h>

#define GDDR_CAL_NUM_ASICS       8
#define GDDR_CAL_NUM_CONTROLLERS 8

/** Widest CA VREFC offset code the 4-bit MR10 field can hold. */
#define GDDR_CA_VREFC_OFFSET_MAX        0xF
/** Widest CA termination offset the MR3 field can hold. */
#define GDDR_CA_TERMINATION_OFFSET_MAX  3
/** Widest CA driver strength code the 3-bit OCD pulldown field can hold. */
#define GDDR_CA_OCD_PULLDOWN_OFFSET_MAX 7
/** CA driver strength sentinel: not seeded, MRISC keeps its board-type default. */
#define GDDR_CA_OCD_PULLDOWN_UNSET      0xFF

/** @brief Per-controller CA calibration entry, latched from MRISC training results. */
typedef struct {
	/** CA VREFC offset as 4-bit two's-complement MR10 OP[3:0] code. */
	uint8_t ca_vrefc_offset;
	/** CA termination offset (MR3, 0-3). */
	uint8_t ca_termination_offset;
	/** CA driver strength as 3-bit OCD pulldown offset code (0-3 = 0..+3, 4-7 = -4..-1). */
	uint8_t ca_ocd_pulldown_offset;
	/** 1 = entry holds latched/provisioned settings, 0 = unset. */
	uint8_t valid;
} gddr_cal_entry_t;

/**
 * @brief GDDR CA calibration table, stored in the gddrcal flash partitions.
 *
 * Entries are indexed [asic][controller], where the ASIC index is the board ASIC
 * location (0 on single-ASIC boards, 0/1 on P300, SPI-provisioned UBB-local
 * location on Galaxy). This lets a single provisioned table image be written to
 * every ASIC's flash; each SMC only reads and updates its own row.
 *
 * This is the payload only. The on-flash framing (magic, sequence number,
 * version, CRC) and the A/B bank protocol that makes updates power-loss safe are
 * private to gddr_cal.c.
 */
typedef struct {
	gddr_cal_entry_t entries[GDDR_CAL_NUM_ASICS][GDDR_CAL_NUM_CONTROLLERS];
} gddr_cal_table_t;

/**
 * @brief Read and validate the CA calibration table from flash.
 *
 * Reads both banks, prefers the one with the newer sequence number, and falls
 * back to the other if it fails validation. Erased banks fail the magic check
 * and are treated as absent, as are banks written by an older firmware whose
 * table layout differed.
 *
 * @param [out] table Destination table; only written on success.
 * @retval 0 on success.
 * @retval -ENOSYS if the gddrcal flash partitions do not exist in the devicetree.
 * @retval -ENODEV if the flash device is not ready.
 * @retval -ENOENT if neither bank holds a valid table.
 */
int gddr_cal_read(gddr_cal_table_t *table);

/**
 * @brief Write the CA calibration table to flash.
 *
 * Writes to whichever bank is not currently active, with a sequence number one
 * greater than the active bank's, then reads the bank back and verifies it. The
 * active bank is never erased, so a power loss at any point leaves the previous
 * table intact and readable.
 *
 * @param [in] table Table to store.
 * @retval 0 on success, negative error code on failure.
 */
int gddr_cal_write(const gddr_cal_table_t *table);

#endif /* GDDR_CAL_H */
