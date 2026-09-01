# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app LANGUAGES C)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/git_version.cmake)

target_sources(app PRIVATE main.c)
if(CONFIG_BOARD_TT_MMK_TT_KERAUNOS_SMC)
    target_sources(app PRIVATE mis_launch_k.c)
elseif(CONFIG_BOARD_TT_MMK_TT_MIMIR_SMC)
    target_sources(app PRIVATE mis_launch_m.c)
else()
    message(FATAL_ERROR "BL1 app is only supported on tt_mmk_tt_keraunos_smc or tt_mmk_tt_mimir_smc")
endif()
