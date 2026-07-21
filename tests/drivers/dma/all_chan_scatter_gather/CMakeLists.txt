# SPDX-License-Identifier: Apache-2.0
set(CMAKE_BUILD_TYPE "Debug")
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(all_chan_scatter_gather)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})
