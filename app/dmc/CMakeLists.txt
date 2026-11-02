# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/git_version.cmake)

project(dmc)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

# status_reg.h is SoC-owned; DMC SOC_FULL_DIR is STM32, so add Blackhole include/.
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../soc/tenstorrent/tt_blackhole/include)
