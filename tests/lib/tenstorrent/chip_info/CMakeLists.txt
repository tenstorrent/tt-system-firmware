# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(chip_info)

set(BH_ARC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../lib/tenstorrent/bh_arc)

target_sources(app PRIVATE
  src/main.c
  ${BH_ARC_DIR}/chip_info_static.c
)
target_include_directories(app PRIVATE ${BH_ARC_DIR})
