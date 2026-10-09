# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/git_version.cmake)

project(dmc)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})
if(CONFIG_TT_QSFP)
  FILE(GLOB qsfp_sources src/qsfp/*.c)
  target_sources(app PRIVATE ${qsfp_sources})
endif()
target_include_directories(app PRIVATE ../../include)
