# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

zephyr_library_amend()
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/memc)
zephyr_library_sources_ifdef(CONFIG_MEMC_TT_MIMIR memc_tt_mimir.c)

if(CONFIG_MEMC_TT_MIMIR)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/sival_sdk.cmake)
  tt_sival_sdk_extract(${CMAKE_CURRENT_SOURCE_DIR}/../../zephyr/blobs/grendel-sival-sdk.tar.gz)
  set(SIVAL_SDK ${SIVAL_EXTDIR}/grendel_sival_sdk)
  set(SIVAL_GDDR_LIB ${SIVAL_SDK}/lib/libdriver_gddr.a)

  if(NOT EXISTS ${SIVAL_GDDR_LIB})
    message(FATAL_ERROR "${SIVAL_GDDR_LIB} not found")
  endif()

  zephyr_library_sources(
    ${SIVAL_SDK}/firmware/libs/gddr/src/gddr_bringup.c
    ${SIVAL_SDK}/firmware/libs/gddr/src/gddr_cmd.c
    ${SIVAL_SDK}/firmware/libs/gddr/src/gddr_irq.c
    ${SIVAL_SDK}/firmware/libs/gddr/src/gddr_lib_service.c
  )
  zephyr_library_include_directories(${SIVAL_SDK}/firmware/libs/gddr/include)
  zephyr_library_link_libraries(${SIVAL_GDDR_LIB})
endif()
