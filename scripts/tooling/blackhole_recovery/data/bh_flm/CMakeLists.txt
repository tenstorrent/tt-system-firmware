# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)

# Locate the Zephyr base / west workspace the same way Zephyr does: prefer the
# ZEPHYR_BASE environment variable that west exports, and only fall back to a
# relative path for standalone (non-west) invocations of this CMake project.
if(DEFINED ENV{ZEPHYR_BASE})
  get_filename_component(ZEPHYR_BASE $ENV{ZEPHYR_BASE} ABSOLUTE)
  get_filename_component(WORKSPACE_ROOT ${ZEPHYR_BASE} DIRECTORY)
else()
  get_filename_component(WORKSPACE_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../../../.. ABSOLUTE)
  set(ZEPHYR_BASE ${WORKSPACE_ROOT}/zephyr)
endif()

# Use Zephyr's SDK finder (version-aware) instead of CMake's config package search.
# Matches west: FindHostTools.cmake calls find_package(Zephyr-sdk 1.0).
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/cmake/modules)
find_package(Zephyr-sdk 1.0 MODULE REQUIRED)

# Set the target compiler
set(CMAKE_C_COMPILER ${ZEPHYR_SDK_INSTALL_DIR}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc)
set(CMAKE_CXX_COMPILER ${ZEPHYR_SDK_INSTALL_DIR}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-g++)
# Add baremetal C compilation flags
string(APPEND CMAKE_C_FLAGS "-Os -Wall -ffunction-sections -fdata-sections ")
string(APPEND CMAKE_C_FLAGS "-fpic -msingle-pic-base -mpic-register=9 ")
string(APPEND CMAKE_C_FLAGS "-fno-jump-tables -ffreestanding -nostartfiles ")
string(APPEND CMAKE_C_FLAGS "-mcpu=cortex-m0 -mthumb ")
string(APPEND CMAKE_C_FLAGS "-DSTM32G0xx -DSTM32G0B1xx -DFLASH_DRV_VERS=0 -DUSE_FULL_LL_DRIVER")

set(COMMON_FLM_DIR ${CMAKE_CURRENT_LIST_DIR}/../common_flm)
string(APPEND CMAKE_EXE_LINKER_FLAGS "-T ${COMMON_FLM_DIR}/FlashAlgo.ld")

project(flm LANGUAGES C)
set(TARGETS spi1.flm spi_combo.flm)
foreach(TARGET ${TARGETS})
  add_executable(${TARGET}
    ${COMMON_FLM_DIR}/eeprom.c
    ${COMMON_FLM_DIR}/FlashPrg.c
    FlashDev.c
    stm32_spi_hal.c
  )
  target_include_directories(${TARGET} PRIVATE ${COMMON_FLM_DIR})
endforeach()

# Set compile definitions for each SPI FLM target
target_compile_definitions(spi1.flm PRIVATE USE_SPI1)
target_compile_definitions(spi_combo.flm PRIVATE USE_SPI_COMBO)

# Include STM32 HAL
set(STM32_MODULE_DIR ${WORKSPACE_ROOT}/modules/hal/stm32)
foreach(TARGET ${TARGETS})
  target_include_directories(${TARGET} PRIVATE ${STM32_MODULE_DIR}/stm32cube/stm32g0xx/soc)
  target_include_directories(${TARGET} PRIVATE ${STM32_MODULE_DIR}/stm32cube/stm32g0xx/drivers/include)
endforeach()

# Include CMSIS HAL
set(CMSIS_MODULE_DIR ${WORKSPACE_ROOT}/modules/hal/cmsis_6)
foreach(TARGET ${TARGETS})
  target_include_directories(${TARGET} PRIVATE ${CMSIS_MODULE_DIR}/CMSIS/Core/Include)
endforeach()
