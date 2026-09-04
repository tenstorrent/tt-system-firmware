# SPDX-License-Identifier: Apache-2.0

# zephyr-keep-sorted-start
add_subdirectory_ifdef(CONFIG_BH_FWTABLE bh_fwtable)
add_subdirectory_ifdef(CONFIG_TT_BH_EFUSE bh_efuse)
add_subdirectory_ifdef(CONFIG_TT_GRENDEL_FILTER tt_grendel_filter)
add_subdirectory_ifdef(CONFIG_TT_MIMIR_REMOTE_BOOT tt_mimir_remote_boot)
add_subdirectory_ifdef(CONFIG_TT_SMC_REMOTEPROC tt_smc_remoteproc)
# zephyr-keep-sorted-stop
