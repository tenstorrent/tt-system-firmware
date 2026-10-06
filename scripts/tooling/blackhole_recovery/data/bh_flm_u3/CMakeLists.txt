# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)

if(DEFINED ENV{ZEPHYR_BASE})
  get_filename_component(ZEPHYR_BASE $ENV{ZEPHYR_BASE} ABSOLUTE)
  get_filename_component(WORKSPACE_ROOT ${ZEPHYR_BASE} DIRECTORY)
else()
  get_filename_component(WORKSPACE_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../../../.. ABSOLUTE)
  set(ZEPHYR_BASE ${WORKSPACE_ROOT}/zephyr)
endif()

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/cmake/modules)
find_package(Zephyr-sdk 1.0 MODULE REQUIRED)

# Cross-compiling: avoid CMake's macOS defaults (e.g. "-arch arm64").
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_OSX_ARCHITECTURES "" CACHE STRING "" FORCE)

set(CMAKE_C_COMPILER ${ZEPHYR_SDK_INSTALL_DIR}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc)
set(CMAKE_CXX_COMPILER ${ZEPHYR_SDK_INSTALL_DIR}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-g++)
string(APPEND CMAKE_C_FLAGS "-Os -Wall -ffunction-sections -fdata-sections ")
string(APPEND CMAKE_C_FLAGS "-fpic -msingle-pic-base -mpic-register=9 ")
string(APPEND CMAKE_C_FLAGS "-fno-jump-tables -ffreestanding -nostartfiles ")
string(APPEND CMAKE_C_FLAGS "-mcpu=cortex-m33 -mthumb ")
string(APPEND CMAKE_C_FLAGS "-DSTM32U3xx -DSTM32U375xx -DFLASH_DRV_VERS=0 -DUSE_FULL_LL_DRIVER")

set(COMMON_FLM_DIR ${CMAKE_CURRENT_LIST_DIR}/../common_flm)
string(APPEND CMAKE_EXE_LINKER_FLAGS "-T ${COMMON_FLM_DIR}/FlashAlgo.ld")

project(flm LANGUAGES C)
add_executable(spi1_u3.flm
  ${COMMON_FLM_DIR}/eeprom.c
  ${COMMON_FLM_DIR}/FlashPrg.c
  ${CMAKE_CURRENT_LIST_DIR}/../bh_flm/FlashDev.c
  stm32u3_spi_hal.c
)
target_include_directories(spi1_u3.flm PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${COMMON_FLM_DIR}
  ${WORKSPACE_ROOT}/modules/hal/stm32/stm32cube/stm32u3xx/soc
  ${WORKSPACE_ROOT}/modules/hal/stm32/stm32cube/stm32u3xx/drivers/include
  ${WORKSPACE_ROOT}/modules/hal/cmsis_6/CMSIS/Core/Include
)
