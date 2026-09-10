# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(memc_tt_mimir)

target_sources(app PRIVATE src/main.c src/chip_init.c src/mimir_gddr_config.c)

target_link_libraries(app PRIVATE gr_smc)
