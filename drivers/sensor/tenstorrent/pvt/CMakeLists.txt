# SPDX-License-Identifier: Apache-2.0

zephyr_library()

# zephyr-keep-sorted-start
zephyr_library_sources(pvt_tt_bh_decoder.c)
zephyr_library_sources_ifdef(CONFIG_PVT_TT_BH pvt_tt_bh.c pvt_tt_bh_rtio.c)
zephyr_library_sources_ifdef(CONFIG_PVT_TT_BH_EMUL pvt_tt_bh_emul.c)
# zephyr-keep-sorted-stop

zephyr_library_include_directories(${ZEPHYR_TT_SYSTEM_FIRMWARE_MODULE_DIR}/lib/tenstorrent/bh_arc)
