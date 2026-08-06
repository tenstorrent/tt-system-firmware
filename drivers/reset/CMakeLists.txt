# SPDX-License-Identifier: Apache-2.0

zephyr_library_amend()

# zephyr-keep-sorted-start
zephyr_library_sources_ifdef(CONFIG_RESET_TT_BH reset_tt_bh.c)
zephyr_library_sources_ifdef(CONFIG_RESET_TT_BH_EMUL reset_tt_bh_emul.c)
# zephyr-keep-sorted-stop
