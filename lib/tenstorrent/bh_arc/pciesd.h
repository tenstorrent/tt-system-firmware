/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TTZP_LIB_TENSTORRENT_BH_ARC_PCIESD_H_
#define TTZP_LIB_TENSTORRENT_BH_ARC_PCIESD_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	PCIeInitOk = 0,
	PCIeSerdesFWLoadTimeout = 1,
	PCIeLinkTrainTimeout = 2,
} PCIeInitStatus;

typedef enum {
	EndPoint = 0,
	RootComplex = 1,
} PCIeDeviceType;

struct CntlInitV2Param {
	uint64_t board_id;
	uint32_t vendor_id;
	uint8_t pcie_inst;
	uint8_t num_serdes_instance;
	uint8_t max_pcie_speed;
	uint8_t device_type;
	uint64_t region0_mask;
	uint64_t region2_mask;
	uint64_t region4_mask;
};

/* The functions below are implemented in tt_blackhole_libpciesd.a */
PCIeInitStatus SerdesInit(uint8_t pcie_inst, PCIeDeviceType device_type,
			  uint8_t num_serdes_instance);
void ExitLoopback(void);
void EnterLoopback(void);
void CntlInit(uint8_t pcie_inst, uint8_t num_serdes_instance, uint8_t max_pcie_speed,
	      uint64_t board_id, uint32_t vendor_id);

void CntlInitV2(const struct CntlInitV2Param *param);
void ChangeLinkSpeed(uint8_t pcie_speed);

#ifdef __cplusplus
}
#endif

#endif /* TTZP_LIB_TENSTORRENT_BH_ARC_PCIESD_H_ */
