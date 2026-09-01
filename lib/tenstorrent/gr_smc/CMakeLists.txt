# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(gr_smc)

zephyr_library_sources_ifdef(CONFIG_TT_GR_SMC_LOG_BRIDGE grendel_log_bridge.c)
