# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(tt_grendel_hsio.c)

if(CONFIG_SOC_TT_KERAUNOS_SMC)
  set(SIVAL_SDK ${CMAKE_BINARY_DIR}/grendel-sival-sdk/grendel_sival_sdk)
  zephyr_library_include_directories(
    ${SIVAL_SDK}/common/include
    ${SIVAL_SDK}/firmware/common/include
    ${SIVAL_SDK}/firmware/libs/clock/include
    ${SIVAL_SDK}/firmware/libs/basic_init/include
    ${SIVAL_SDK}/firmware/drivers/firewall/include
    ${SIVAL_SDK}/firmware/drivers/smc/include
  )
  zephyr_library_sources(
    ${SIVAL_SDK}/firmware/libs/clock/src/clock.c
    ${SIVAL_SDK}/firmware/drivers/smc/src/smc_init.c
  )
endif()
