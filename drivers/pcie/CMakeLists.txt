# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

set(SIVAL_SDK ${CMAKE_BINARY_DIR}/grendel-sival-sdk/grendel_sival_sdk)
set(SIVAL_PCIE_LIB ${SIVAL_SDK}/lib/libdriver_pcie.a)

if(NOT EXISTS ${SIVAL_PCIE_LIB})
  message(FATAL_ERROR "${SIVAL_PCIE_LIB} not found")
endif()

zephyr_library()

zephyr_library_include_directories(
  ${SIVAL_SDK}/common/include
  ${SIVAL_SDK}/firmware/drivers/pcie/include
  ${SIVAL_SDK}/firmware/common/include
  ${SIVAL_SDK}/firmware/drivers/addr_remap/include
  ${SIVAL_SDK}/firmware/libs/basic_init/include
  ${SIVAL_SDK}/firmware/drivers/smc/include
  ${SIVAL_SDK}/firmware/drivers/firewall/include
)

zephyr_library_sources(tt_grendel_pcie.c)
zephyr_library_sources(
  ${SIVAL_SDK}/firmware/drivers/addr_remap/src/remapper.c
  ${SIVAL_SDK}/firmware/libs/basic_init/src/basic_init.c
)
zephyr_library_link_libraries(${SIVAL_PCIE_LIB})
