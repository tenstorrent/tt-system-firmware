# SPDX-License-Identifier: Apache-2.0

# zephyr-keep-sorted-start
add_subdirectory_ifdef(CONFIG_BH_FWTABLE bh_fwtable)
add_subdirectory_ifdef(CONFIG_TT_OCCP_I3C tt_occp_i3c)
add_subdirectory_ifdef(CONFIG_TT_SMC_REMOTEPROC tt_smc_remoteproc)
# zephyr-keep-sorted-stop
