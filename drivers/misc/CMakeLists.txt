# SPDX-License-Identifier: Apache-2.0

# zephyr-keep-sorted-start
add_subdirectory_ifdef(CONFIG_BH_FWTABLE bh_fwtable)
add_subdirectory_ifdef(CONFIG_DT_HAS_TENSTORRENT_GRENDEL_AXI_FILTER_ENABLED tt_grendel_filters)
add_subdirectory_ifdef(CONFIG_TT_BH_EFUSE bh_efuse)
add_subdirectory_ifdef(CONFIG_TT_SMC_REMOTEPROC tt_smc_remoteproc)
# zephyr-keep-sorted-stop
