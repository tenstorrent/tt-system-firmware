# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tt_pcie)

target_sources(app PRIVATE src/main.c src/stage_bundle.c)

# Embed the real SERDES ICCM/DCCM images from the SiVal SDK
set(SIVAL_BLOBS_TARBALL ${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/blobs/grendel-sival-sdk.tar.gz)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/sival_sdk.cmake)
tt_sival_sdk_extract(${SIVAL_BLOBS_TARBALL})
set(SERDES_FW_DIR ${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/drivers/pcie/fw)

set(GEN_DIR ${ZEPHYR_BINARY_DIR}/include/generated)
foreach(mem iccm dccm)
  set(fw ${SERDES_FW_DIR}/serdes_${mem}_fw.bin)
  file(SIZE ${fw} fw_size)
  if(fw_size GREATER 65536)
    message(FATAL_ERROR "${fw} is ${fw_size} bytes; the driver reserves 64 KiB for ${mem}")
  endif()
  generate_inc_file_for_target(app ${fw} ${GEN_DIR}/serdes_${mem}_fw.bin.inc)
endforeach()
