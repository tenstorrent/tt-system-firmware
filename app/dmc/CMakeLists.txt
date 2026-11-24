# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/git_version.cmake)

project(dmc)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

if(CONFIG_DMC_TEST_SKIP_IMAGE_CONFIRM)
  message(WARNING "CONFIG_DMC_TEST_SKIP_IMAGE_CONFIRM is set: this image never confirms "
                  "itself and MCUboot reverts it at the next reset. Test builds only.")
endif()

# status_reg.h is SoC-owned; DMC SOC_FULL_DIR is STM32, so add Blackhole include/.
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../soc/tenstorrent/tt_blackhole/include)
