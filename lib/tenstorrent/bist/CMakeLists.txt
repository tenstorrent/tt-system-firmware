# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_SOC_TT_BLACKHOLE_DMC tt_bist_bh_dmc.c)
zephyr_library_sources_ifdef(CONFIG_BOARD_TT_BLACKHOLE_GLX2_DMC tt_bist_bh_dmc.c)
