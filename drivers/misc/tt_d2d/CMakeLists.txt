# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_TT_D2D tt_d2d.c)

# Include closed source SiVal drop.
if(CONFIG_TT_D2D)
  set(SIVAL_BLOBS_TARBALL
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/blobs/grendel-sival-sdk.tar.gz)

  if(NOT EXISTS "${SIVAL_BLOBS_TARBALL}")
    message(FATAL_ERROR
            "The Sival SDK drop is missing:\n  ${SIVAL_BLOBS_TARBALL}\n"
            "It is closed source and distributed separately.")
  endif()

  include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/sival_sdk.cmake)
  tt_sival_sdk_extract(${SIVAL_BLOBS_TARBALL})

  zephyr_include_directories(
    ${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/drivers/d2d/include)
endif()
