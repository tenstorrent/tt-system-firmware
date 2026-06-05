# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bh_fwtable_sim)

target_sources(app PRIVATE src/main.c)

# Copy the prepared tt-boot-fs blob into the build directory so the flash
# simulator finds it via its default lookup (./flash.bin, relative to cwd).
# This makes the test work under twister without -- --flash=... forwarding.
set(FLASH_BIN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/flash.bin")
if(EXISTS "${FLASH_BIN_SRC}")
    configure_file("${FLASH_BIN_SRC}" "${CMAKE_BINARY_DIR}/flash.bin" COPYONLY)
else()
    message(WARNING "flash.bin not found at ${FLASH_BIN_SRC}. "
                    "Generate it with: "
                    "python3 scripts/tt_boot_fs.py mkfs "
                    "tests/lib/tenstorrent/bh_fwtable/flash_spec.yaml "
                    "tests/lib/tenstorrent/bh_fwtable/flash.bin")
endif()