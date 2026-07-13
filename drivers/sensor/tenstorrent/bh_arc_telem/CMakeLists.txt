# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_include_directories_ifdef(CONFIG_BH_ARC_TELEMETRY
    ${CMAKE_CURRENT_LIST_DIR}/../../../../lib/tenstorrent/bh_arc)

zephyr_library_sources_ifdef(CONFIG_BH_ARC_TELEMETRY bh_arc_telem.c)
