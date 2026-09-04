# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(tt_grendel_filter.c)
zephyr_library_sources_ifdef(CONFIG_GRENDEL_FILTER_INIT tt_grendel_filter_init.c)
