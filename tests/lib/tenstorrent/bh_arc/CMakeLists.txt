# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bh_arc)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})
target_link_libraries(app PRIVATE bh_fwtable)
target_include_directories(app PRIVATE ../../../../include)
target_include_directories(app PRIVATE ../../../../lib/tenstorrent/bh_arc)

# Copy the prepared tt-boot-fs blob into the build directory so the flash
# simulator finds it via its default lookup (./flash.bin, relative to cwd).
# This makes test_msg_type_force_vdd (and any future test that reads the
# fw_table) work under twister without -- --flash=... forwarding.
set(FLASH_BIN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/flash.bin")
if(EXISTS "${FLASH_BIN_SRC}")
    configure_file("${FLASH_BIN_SRC}" "${CMAKE_BINARY_DIR}/flash.bin" COPYONLY)
else()
    message(WARNING "flash.bin not found at ${FLASH_BIN_SRC}. "
                    "Generate it from the repo root with: "
                    "python3 scripts/tt_boot_fs.py mkfs "
                    "tests/lib/tenstorrent/bh_arc/flash_spec.yaml "
                    "tests/lib/tenstorrent/bh_arc/flash.bin "
                    "(tests that depend on fw_table data will SKIP without it)")
endif()
