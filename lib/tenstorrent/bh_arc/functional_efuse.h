/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FUNCTIONAL_EFUSE_H
#define FUNCTIONAL_EFUSE_H

#include <stdint.h>
#include <zephyr/sys/util.h>

#define FUSE_ASIC_ID_OLD_START_BIT                    1056
#define FUSE_ASIC_ID_OLD_END_BIT                      1071
#define FUSE_ASIC_ID_LOW_START_BIT                    1600
#define FUSE_ASIC_ID_LOW_END_BIT                      1631
/* Functional efuse word 0x3D. Fused as the FT test program revision, used for substrate version */
#define FUSE_SUBSTRATE_VERSION_START_BIT              1952
#define FUSE_SUBSTRATE_VERSION_END_BIT                1983
#define FUSE_ATE_TENSIX_ROW0_TEST_STATUS_START_BIT    3168
#define FUSE_ATE_TENSIX_ROW0_TEST_STATUS_END_BIT      3199
#define FUSE_ATE_TENSIX_ROW1_TEST_STATUS_START_BIT    3200
#define FUSE_ATE_TENSIX_ROW1_TEST_STATUS_END_BIT      3231
#define FUSE_ATE_TENSIX_ROW2_TEST_STATUS_START_BIT    3232
#define FUSE_ATE_TENSIX_ROW2_TEST_STATUS_END_BIT      3263
#define FUSE_ATE_TENSIX_ROW3_TEST_STATUS_START_BIT    3264
#define FUSE_ATE_TENSIX_ROW3_TEST_STATUS_END_BIT      3295
#define FUSE_ATE_TENSIX_ROW4_TEST_STATUS_START_BIT    3296
#define FUSE_ATE_TENSIX_ROW4_TEST_STATUS_END_BIT      3327
#define FUSE_ATE_TENSIX_ROW5_TEST_STATUS_START_BIT    3328
#define FUSE_ATE_TENSIX_ROW5_TEST_STATUS_END_BIT      3359
#define FUSE_ATE_TENSIX_ROW6_TEST_STATUS_START_BIT    3360
#define FUSE_ATE_TENSIX_ROW6_TEST_STATUS_END_BIT      3391
#define FUSE_ATE_TENSIX_ROW7_TEST_STATUS_START_BIT    3392
#define FUSE_ATE_TENSIX_ROW7_TEST_STATUS_END_BIT      3423
#define FUSE_ATE_TENSIX_ROW8_TEST_STATUS_START_BIT    3424
#define FUSE_ATE_TENSIX_ROW8_TEST_STATUS_END_BIT      3455
#define FUSE_ATE_TENSIX_ROW9_TEST_STATUS_START_BIT    3456
#define FUSE_ATE_TENSIX_ROW9_TEST_STATUS_END_BIT      3487
#define FUSE_ATE_SERDES_PHY_TEST_STATUS_START_BIT     3488
#define FUSE_ATE_SERDES_PHY_TEST_STATUS_END_BIT       3503
#define FUSE_ATE_DDR_TEST_STATUS_START_BIT            3504
#define FUSE_ATE_DDR_TEST_STATUS_END_BIT              3511
#define FUSE_ATE_PCIE_SPEED_TEST_START_BIT            3512
#define FUSE_ATE_PCIE_SPEED_TEST_END_BIT              3515
#define FUSE_ATE_AT_SPEED_AICLK_TEST_STATUS_START_BIT 3516
#define FUSE_ATE_AT_SPEED_AICLK_TEST_STATUS_END_BIT   3519
#define FUSE_ATE_RISCV_L2_TEST_STATUS_START_BIT       3520
#define FUSE_ATE_RISCV_L2_TEST_STATUS_END_BIT         3535
#define FUSE_ATE_ETH_CTRL_TEST_STATUS_START_BIT       3536
#define FUSE_ATE_ETH_CTRL_TEST_STATUS_END_BIT         3551
#define FUSE_SLT_ATE_SLT_STATUS_START_BIT             4160
#define FUSE_SLT_ATE_SLT_STATUS_END_BIT               4161
#define FUSE_SLT_FUSE_MAP_VERSION_START_BIT           4162
#define FUSE_SLT_FUSE_MAP_VERSION_END_BIT             4191
#define FUSE_SLT_SLT_BINNING_START_BIT                4192
#define FUSE_SLT_SLT_BINNING_END_BIT                  4223
#define FUSE_PROCESS_RO_START_BIT                     4464
#define FUSE_PROCESS_RO_END_BIT                       4479
#define FUSE_SLT_DDR_TEST_STATUS_16G_START_BIT        5472
#define FUSE_SLT_DDR_TEST_STATUS_16G_END_BIT          5479
#define FUSE_SLT_DDR_TEST_STATUS_14G_START_BIT        5480
#define FUSE_SLT_DDR_TEST_STATUS_14G_END_BIT          5487
#define FUSE_SLT_DDR_TEST_STATUS_12G_START_BIT        5488
#define FUSE_SLT_DDR_TEST_STATUS_12G_END_BIT          5495
#define FUSE_SLT_PCIE_SPEED_TEST_START_BIT            5496
#define FUSE_SLT_PCIE_SPEED_TEST_END_BIT              5499
#define FUSE_SLT_SLT_SPEED_TEST_START_BIT             5500
#define FUSE_SLT_SLT_SPEED_TEST_END_BIT               5503
#define FUSE_SLT_RISCV_L2_TEST_STATUS_START_BIT       5504
#define FUSE_SLT_RISCV_L2_TEST_STATUS_END_BIT         5519
#define FUSE_SLT_ETH_CTRL_TEST_STATUS_START_BIT       5520
#define FUSE_SLT_ETH_CTRL_TEST_STATUS_END_BIT         5535
#define FUSE_SLT_HARVESTED_TENSIX_COLUMNS_START_BIT   5536
#define FUSE_SLT_HARVESTED_TENSIX_COLUMNS_END_BIT     5551
#define FUSE_ASIC_ID_HIGH_START_BIT                   5568
#define FUSE_ASIC_ID_HIGH_END_BIT                     5599

/* Use this helper macro to read functional efuse fields */
/* Note that it only works for fields that are 32-bits or smaller */
#define READ_FUNCTIONAL_EFUSE(fuse_name)                                                           \
	(ZERO_OR_COMPILE_ERROR(FUSE_##fuse_name##_END_BIT > FUSE_##fuse_name##_START_BIT) +        \
	 ZERO_OR_COMPILE_ERROR(FUSE_##fuse_name##_END_BIT - FUSE_##fuse_name##_START_BIT < 32) +   \
	 ReadFunctionalEfuse(FUSE_##fuse_name##_START_BIT, FUSE_##fuse_name##_END_BIT))

#ifdef __cplusplus
extern "C" {
#endif

uint32_t ReadFunctionalEfuse(uint32_t start_bit, uint32_t end_bit);

#ifdef __cplusplus
}
#endif

#endif
