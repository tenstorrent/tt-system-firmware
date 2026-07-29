# SPDX-License-Identifier: Apache-2.0

if(CONFIG_PVT_TT_BH OR CONFIG_PVT_TT_BH_EMUL)
# zephyr-keep-sorted-start
add_subdirectory(pvt)
# zephyr-keep-sorted-stop
endif()

add_subdirectory_ifdef(CONFIG_BH_ARC_TELEMETRY bh_arc_telem)
