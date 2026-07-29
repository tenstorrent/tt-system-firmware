# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app LANGUAGES C)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/git_version.cmake)

target_include_directories(app PRIVATE ${ZEPHYR_TT_SYSTEM_FIRMWARE_MODULE_DIR}/lib/tenstorrent/bh_arc)

if(CONFIG_BH_FWTABLE)
  target_include_directories(app PRIVATE ${CMAKE_BINARY_DIR}/modules/tt-system-firmware/drivers/misc/bh_fwtable/spirom_protobufs)
  add_dependencies(app nanopb_generated_headers)
endif()

target_sources(app PRIVATE src/main.c)
