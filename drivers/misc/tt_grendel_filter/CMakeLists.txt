# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(tt_grendel_filter.c)
zephyr_library_sources_ifdef(CONFIG_SOC_TT_KERAUNOS_SMC tt_grendel_filter_init.c)

if(CONFIG_SOC_TT_KERAUNOS_SMC)
  set(SIVAL_SDK ${CMAKE_BINARY_DIR}/grendel-sival-sdk/grendel_sival_sdk)
  # scratch.h pulls fw_postcodes.h, which the SOC include paths do not cover.
  zephyr_library_include_directories(
    ${SIVAL_SDK}/common/include
    ${SIVAL_SDK}/firmware/common/include
    ${SIVAL_SDK}/firmware/drivers/firewall/include
  )
  zephyr_library_sources(${SIVAL_SDK}/firmware/drivers/firewall/src/firewall.c)
endif()
