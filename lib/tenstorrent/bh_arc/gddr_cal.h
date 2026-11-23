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
/** PHY CA TX drive code range (PHYCMN_ZQCALCONFIG3.drv_config_tx_ca, 6 bits, 0 not valid). */
#define GDDR_CA_PHY_DRV_TX_CA_MIN       1
#define GDDR_CA_PHY_DRV_TX_CA_MAX       0x3F
/** PHY CA TX drive sentinel: not seeded, MRISC keeps its board default. */
#define GDDR_CA_PHY_DRV_TX_CA_UNSET     0xFF
/** CA phase offset sentinel: not seeded, MRISC applies no offset to the CA training result. */
#define GDDR_CA_PHASE_OFFSET_UNSET      INT8_MIN
/** Channel B CA VREFC sentinel: same as ca_vrefc_offset. */
#define GDDR_CA_VREFC_OFFSET_CH1_SAME   0xFF

/** @brief Per-controller CA calibration entry, latched from MRISC training results. */
typedef struct {
	/** CA VREFC offset as 4-bit two's-complement MR10 OP[3:0] code (channel A, and B unless
	 *  ca_vrefc_offset_ch1 is set).
	 */
	uint8_t ca_vrefc_offset;
	/** CA termination offset (MR3, 0-3). */
	uint8_t ca_termination_offset;
	/** CA drive strength: PHY CA TX drive code (PHYCMN_ZQCALCONFIG3.drv_config_tx_ca,
	 *  1-0x3F), or @ref GDDR_CA_PHY_DRV_TX_CA_UNSET. In v3 this byte held the DRAM OCD
	 *  pulldown offset, which MRISC 2.18 no longer uses as a CA setting.
	 */
	uint8_t ca_phy_drv_tx_ca;
	/** 1 = entry holds latched/provisioned settings, 0 = unset. */
	uint8_t valid;
	/** Per-channel CA phase offset on top of CA training, in units of 2 mixer steps
	 *  (MRISC 2.18+ 2D eye centering). @ref GDDR_CA_PHASE_OFFSET_UNSET = not seeded.
	 */
	int8_t ca_phase_offset[2];
	/** Channel B CA VREFC offset code, or @ref GDDR_CA_VREFC_OFFSET_CH1_SAME. */
	uint8_t ca_vrefc_offset_ch1;
	/** Reserved, 0. */
	uint8_t reserved;
} gddr_cal_entry_t;

/**
 * @brief GDDR CA calibration table, stored in the gddrcal flash partitions.
 *
 * Entries are indexed [asic][controller], where the ASIC index is the board ASIC
 * location (0 on single-ASIC boards, 0/1 on P300, SPI-provisioned UBB-local
 * location minus one on Galaxy, where locations are 1-8). This lets a single
 * provisioned table image be written to every ASIC's flash; each SMC only reads
 * and updates its own row.
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
 * table layout differed. The exception is the v3 layout, which is converted on
 * read: the DRAM OCD byte it kept where v4 has the PHY CA TX drive comes back as
 * an unset drive, the phase offsets come back unset and channel B's VREFC follows
 * channel A, and the next write stores the v4 layout.
 *
 * CONFIG_TT_BH_ARC_GDDR_CA_LATCH requires gddrcal_a and gddrcal_b in the
 * devicetree; a build without those nodes is rejected at compile time.
 *
 * @param [out] table Destination table; only written on success.
 * @retval 0 on success.
 * @retval -ENODEV if the flash device is not ready.
 * @retval -ENOENT if neither bank holds a valid table.
 * @retval -ENOSYS if the latch Kconfig is off and the partitions are absent.
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
 * @retval -ENOSYS if the latch Kconfig is off and the partitions are absent.
 */
int gddr_cal_write(const gddr_cal_table_t *table);

#endif /* GDDR_CAL_H */
