# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gddr_cal)

set(TT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

target_sources(app PRIVATE
  src/main.c
  ${TT_ROOT}/lib/tenstorrent/bh_arc/gddr_cal.c
)
target_include_directories(app PRIVATE ${TT_ROOT}/lib/tenstorrent/bh_arc)
