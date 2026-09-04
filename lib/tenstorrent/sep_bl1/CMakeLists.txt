# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(sep_bl1.c)
zephyr_library_sources_ifdef(CONFIG_SOC_TT_KERAUNOS_SEP sep_keraunos.c)
