/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TT_GRENDEL_HSIO_SIVAL_ADDRS_H_
#define TT_GRENDEL_HSIO_SIVAL_ADDRS_H_

#include <stdint.h>

uint32_t tt_sival_ker_rst_hsio0(void);
uint32_t tt_sival_smc_cold_reset_n_addr(void);
uint32_t tt_sival_hsio0_cold_reset_n_addr(void);
uint32_t tt_sival_hsio0_ag_mux_addr(void);
uint64_t tt_sival_smn_hsio2hsio_filter_base(void);
uint64_t tt_sival_smn_hsio2smn_filter_base(void);
uint64_t tt_sival_hsio_noc2axi_filter_base(void);
uint64_t tt_sival_hsio_pcie_filter_base(void);
uint32_t tt_sival_filter_stride(void);
uint32_t tt_sival_filter_start_offset(void);
uint32_t tt_sival_filter_end_offset(void);
uint64_t tt_sival_firewall_addr_mask(void);

#endif
