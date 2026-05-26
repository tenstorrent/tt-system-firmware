/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CCFGOVR_H
#define CCFGOVR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CCFGOVR_TAG_A "ccfgovra"
#define CCFGOVR_TAG_B "ccfgovrb"

#define CCFGOVR_MAGIC      0x564F4343U
#define CCFGOVR_SEQ_ERASED 0xFFFFFFFFU

#define CCFGOVR_MAX_BANK_LEN 4096U

/*
 * Bump on any layout-changing edit to struct ccfgovr_bank_hdr
 * or to the on-flash framing.
 */
#define CCFGOVR_HDR_VERSION 0U

struct ccfgovr_bank_hdr {
	uint32_t magic;    /* must equal CCFGOVR_MAGIC */
	uint32_t seq;      /* monotonic; CCFGOVR_SEQ_ERASED is reserved */
	uint32_t body_len; /* protobuf body length in bytes; multiple of 4 */
	uint32_t version;  /* reserved; must equal CCFGOVR_HDR_VERSION */
	uint32_t cksum;    /* CRC32 (IEEE 802.3) over hdr[0 .. offsetof(cksum)) || body[] */
};

#define CCFGOVR_MAX_BODY_LEN (CCFGOVR_MAX_BANK_LEN - sizeof(struct ccfgovr_bank_hdr))

#ifdef __cplusplus
}
#endif

#endif /* CCFGOVR_H */
