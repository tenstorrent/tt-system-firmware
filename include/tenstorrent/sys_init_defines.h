/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TENSTORRENT_SYS_INIT_DEFINES_H_
#define TENSTORRENT_SYS_INIT_DEFINES_H_

#include <zephyr/init.h>

/* SYS_INIT POST_KERNEL defines */
#define register_interrupt_handlers_PRIO      90
#define arc_dma_init_PRIO                     91
#define InitSpiFS_PRIO                        92
#define bh_arc_init_start_PRIO                93
#define CATEarlyInit_PRIO                     94
#define CalculateHarvesting_PRIO              95
#define DeassertTileResets_PRIO               96
#define PLLInit_PRIO                          97
#define PVTInit_PRIO                          98
#define NocInit_PRIO                          99
#define AssertSoftResets_PRIO                 100
#define DeassertRiscvResets_PRIO              101
#define InitAiclkPPM_PRIO                     102
#define pcie_init_PRIO                        103
#define tensix_init_PRIO                      104
#define InitMrisc_PRIO                        105
#define eth_init_PRIO                         106
#define InitSmbusTarget_PRIO                  107
#define regulator_init_PRIO                   108
#define avs_init_PRIO                         109
#define InitNocTranslationFromHarvesting_PRIO 110
#define gddr_training_PRIO                    111
#define CATInit_PRIO                          112
#define init_msgqueue_PRIO                    113
#define publish_runtime_telemetry_PRIO        114
#define bh_arc_init_end_PRIO                  115

#define SYS_INIT_APP(func) SYS_INIT(func, POST_KERNEL, func##_PRIO)

#endif
