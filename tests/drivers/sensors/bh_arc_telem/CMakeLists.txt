# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bh_arc_telem_tests)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../../lib/tenstorrent/bh_arc)
target_sources(app PRIVATE src/main.c)
