/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PCIE_LTSSM_LOG_H
#define PCIE_LTSSM_LOG_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

/* Raw smlh_ltssm_state_sync values; host readers: syseng scripts/pcie/ltssm_dump/ */
enum ltssm_state {
	LTSSM_STATE_DETECT_QUIET = 0x00,
	LTSSM_STATE_L0 = 0x11,
	LTSSM_STATE_NONE = 0x3F, /* not a hardware state: nothing recorded yet */
};

/* Bit layout of struct ltssm_log_entry::info */
#define LTSSM_INFO_STATE_MASK   0x0000003FU
#define LTSSM_INFO_STATE_SHIFT  0
#define LTSSM_INFO_LINK_UP      BIT(6)
#define LTSSM_INFO_RDLH_LINK_UP BIT(7)
#define LTSSM_INFO_INST         BIT(8)
#define LTSSM_INFO_REPEAT_SHIFT 9
#define LTSSM_INFO_REPEAT_MAX   0x007FFFFFU /* 23 bits, saturating */
/* Fields that must match for two observations to share an entry */
#define LTSSM_INFO_ID_MASK                                                                         \
	(LTSSM_INFO_STATE_MASK | LTSSM_INFO_LINK_UP | LTSSM_INFO_RDLH_LINK_UP | LTSSM_INFO_INST)

/* Bit layout of struct ltssm_log_entry::cdr; LANES == 0 means the PHY was not sampled */
#define LTSSM_CDR_LOCK_MASK   0x0000FFFFU
#define LTSSM_CDR_LANES_MASK  0x001F0000U
#define LTSSM_CDR_LANES_SHIFT 16
#define LTSSM_CDR_MAX_LANES   16

struct ltssm_log_entry {
	uint64_t timestamp;  /* refclk ticks at the first observation */
	uint32_t info;       /* see LTSSM_INFO_* */
	uint32_t last_delta; /* ticks from timestamp to the last observation, saturating */
	uint32_t cdr;        /* see LTSSM_CDR_*; part of the entry's identity */
	uint32_t rsvd;       /* explicit pad to 24 bytes */
};

#define LTSSM_LOG_MAGIC   0x4D53544CU /* "LTSM" */
#define LTSSM_LOG_VERSION 3U          /* bump on any layout change; host readers check it */
#define LTSSM_LOG_TICK_HZ 50000000U   /* 50 MHz refclk, see WAIT_1MS in timer.h */

/* Followed by capacity entries; magic and version stay first so old readers reject cleanly */
struct ltssm_log_header {
	uint32_t magic;
	uint32_t version;
	uint32_t entry_size;
	uint32_t capacity;
	uint32_t count;   /* entries held, at most capacity */
	uint32_t dropped; /* entries evicted by the ring wrapping */
	uint32_t tick_hz;
	uint32_t head; /* slot the next entry goes to; oldest is (head - count) mod capacity */
};

/** @brief Clear the log and publish its address and size in scratch 24/25. */
void ltssm_log_reset(void);

/** @brief Record an observation, folding repeats and two-state alternations in place. */
void ltssm_log_record(uint8_t pcie_inst, uint8_t state, bool link_up, bool rdlh_link_up,
		      uint32_t cdr, uint64_t timestamp);

#endif /* PCIE_LTSSM_LOG_H */
