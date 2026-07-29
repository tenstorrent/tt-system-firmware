# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

zephyr_library_amend()

# zephyr-keep-sorted-start
zephyr_library_sources_ifdef(CONFIG_DMA_ARC_HS dma_arc_hs.c)
zephyr_library_sources_ifdef(CONFIG_DMA_TT_BH_NOC dma_tt_bh_noc.c)
zephyr_library_sources_ifdef(CONFIG_DMA_TT_GRENDEL_SMC dma_tt_grendel_smc.c)
# zephyr-keep-sorted-stop

zephyr_library_include_directories_ifdef(CONFIG_DMA_TT_BH_NOC
  ${ZEPHYR_TT_SYSTEM_FIRMWARE_MODULE_DIR}/lib/tenstorrent/bh_arc)

zephyr_syscall_header_ifdef(CONFIG_DMA_TT_BH_NOC
  ${ZEPHYR_TT_SYSTEM_FIRMWARE_MODULE_DIR}/include/zephyr/drivers/dma/dma_tt_bh_noc.h)
