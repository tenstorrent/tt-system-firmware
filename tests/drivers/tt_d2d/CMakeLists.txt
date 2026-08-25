# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# By default the test stands in a generated image of its own. The real D2D
# firmware arrives in the Sival drop and is not in this tree.
#
# Pass -DD2D_FW_BIN=<path> to run against the real image instead.
if(D2D_FW_BIN)
  set(TT_D2D_FW_BLOB_FILE "${D2D_FW_BIN}")
else()
  set(D2D_FW_STUB_UNIT "TT-D2D-FW-STUB..")
  set(D2D_FW_STUB_REPEAT 16)
  set(TT_D2D_FW_BLOB_FILE "${CMAKE_CURRENT_BINARY_DIR}/d2d_fw_stub.bin")

  string(REPEAT "${D2D_FW_STUB_UNIT}" ${D2D_FW_STUB_REPEAT} d2d_fw_stub_bytes)
  file(WRITE "${TT_D2D_FW_BLOB_FILE}" "${d2d_fw_stub_bytes}")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tt_d2d)

target_sources(app PRIVATE src/main.c src/chip_init.c)

# So the test checks the image it generated rather than a copy of the pattern
# that could drift from it. Absent when running against a real image, whose
# contents the test has no business asserting.
if(D2D_FW_STUB_UNIT)
  target_compile_definitions(app PRIVATE
                             D2D_FW_STUB_UNIT="${D2D_FW_STUB_UNIT}"
                             D2D_FW_STUB_REPEAT=${D2D_FW_STUB_REPEAT})
endif()
