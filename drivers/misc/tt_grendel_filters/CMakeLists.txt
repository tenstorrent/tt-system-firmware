# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(tt_grendel_axi_filter.c)
zephyr_library_sources_ifdef(CONFIG_SOC_TT_KERAUNOS_SMC tt_grendel_smc_filter_init.c)
