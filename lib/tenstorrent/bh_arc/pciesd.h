/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TTZP_LIB_TENSTORRENT_BH_ARC_PCIESD_H_
#define TTZP_LIB_TENSTORRENT_BH_ARC_PCIESD_H_

#include "arc_dma.h"
#include "pcie.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CntlInitV2Param {
	uint64_t board_id;
	uint32_t vendor_id;
	uint8_t pcie_inst;
	uint8_t serdes_inst;
	uint8_t max_pcie_speed;
	uint8_t device_type;
	uint64_t region0_mask;
	uint64_t region2_mask;
	uint64_t region4_mask;
	uint32_t gen3_eq_pset_req_vec;
	uint8_t gen3_eq_fb_mode;
};

/* Verify prototype of ArcDmaTransfer, because it's used by libpciesd.a. */
static __unused bool (*verify_ArcDmaTransfer)(const void *, void *, uint32_t) = ArcDmaTransfer;

/* The functions below are implemented in tt_blackhole_libpciesd.a */
PCIeInitStatus SerdesInit(uint8_t pcie_inst, PCIeDeviceType device_type,
			  uint8_t num_serdes_instance);
void ExitLoopback(void);
void EnterLoopback(void);
void CntlInit(uint8_t pcie_inst, uint8_t num_serdes_instance, uint8_t max_pcie_speed,
	      uint64_t board_id, uint32_t vendor_id);

void CntlInitV2(const struct CntlInitV2Param *param);

#ifdef __cplusplus
}
#endif

#endif /* TTZP_LIB_TENSTORRENT_BH_ARC_PCIESD_H_ */
