/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TT_GRENDEL_FILTER_SIVAL_ADDRS_H_
#define TT_GRENDEL_FILTER_SIVAL_ADDRS_H_

#include <stdint.h>

uint64_t tt_sival_inbound_filter_base(void);
uint32_t tt_sival_smc_inbound(void);
uint32_t tt_sival_filter_stride(void);
uint32_t tt_sival_filter_config_offset(void);
uint32_t tt_sival_filter_start_offset(void);
uint32_t tt_sival_filter_end_offset(void);

#endif
