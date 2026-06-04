# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(occp)

target_sources_ifdef(CONFIG_OCCP_BACKEND_I3C app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_OCCP_BACKEND_FAKE app PRIVATE src/fake_tests.c)
