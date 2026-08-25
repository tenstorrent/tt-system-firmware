# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

set(SIVAL_SDK ${CMAKE_BINARY_DIR}/grendel-sival-sdk/grendel_sival_sdk)
set(SIVAL_PCIE_LIB ${SIVAL_SDK}/lib/libdriver_pcie.a)

if(NOT EXISTS ${SIVAL_PCIE_LIB})
  message(FATAL_ERROR "${SIVAL_PCIE_LIB} not found")
endif()

zephyr_library()

zephyr_library_include_directories(
  ${SIVAL_SDK}/firmware/drivers/pcie/include
  ${SIVAL_SDK}/firmware/common/include
)

zephyr_library_sources(tt_grendel_pcie.c)
zephyr_library_link_libraries(${SIVAL_PCIE_LIB})
