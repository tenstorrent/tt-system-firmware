# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# By default the test stands in a generated image of its own. The real D2D
# firmware arrives in the Sival drop and is not in this tree.
#
# Pass -DD2D_FW_BIN=<path> to run against the real image instead.
if(D2D_FW_BIN)
  set(D2D_FW_BLOB_FILE "${D2D_FW_BIN}")

  if(NOT IS_ABSOLUTE "${D2D_FW_BLOB_FILE}")
    set(D2D_FW_BLOB_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${D2D_FW_BLOB_FILE}")
  endif()

  if(NOT EXISTS "${D2D_FW_BLOB_FILE}")
    message(FATAL_ERROR
            "D2D_FW_BIN names no such file:\n  ${D2D_FW_BLOB_FILE}\n"
            "The image ships in the Sival tarball drop.")
  endif()

  # tt_d2d_load_fw() copies the image a word at a time and rejects a length
  # that is not a multiple of four. Catching that here names the offending
  # file; left to run time it is an -EINVAL from a call whose arguments look
  # fine.
  file(SIZE "${D2D_FW_BLOB_FILE}" d2d_fw_blob_size)
  math(EXPR d2d_fw_blob_tail "${d2d_fw_blob_size} % 4")
  if(NOT d2d_fw_blob_tail EQUAL 0)
    message(FATAL_ERROR
            "D2D firmware image is ${d2d_fw_blob_size} bytes, which is not a "
            "multiple of 4 and cannot be loaded:\n  ${D2D_FW_BLOB_FILE}")
  endif()
else()
  set(D2D_FW_STUB_UNIT "TT-D2D-FW-STUB..")
  set(D2D_FW_STUB_REPEAT 16)
  set(D2D_FW_BLOB_FILE "${CMAKE_CURRENT_BINARY_DIR}/d2d_fw_stub.bin")

  string(REPEAT "${D2D_FW_STUB_UNIT}" ${D2D_FW_STUB_REPEAT} d2d_fw_stub_bytes)
  file(WRITE "${D2D_FW_BLOB_FILE}" "${d2d_fw_stub_bytes}")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tt_d2d)

target_sources(app PRIVATE src/main.c src/chip_init.c src/d2d_fw_blob.c)

# Turns the image into byte initialisers for d2d_fw_blob.c to include. The
# generated file depends on the image, so a new drop rebuilds it.
set(gen_dir ${ZEPHYR_BINARY_DIR}/misc/generated)
generate_inc_file_for_target(app "${D2D_FW_BLOB_FILE}" ${gen_dir}/d2d_fw.inc)
target_include_directories(app PRIVATE ${gen_dir})

# So the test checks the image it generated rather than a copy of the pattern
# that could drift from it. Absent when running against a real image, whose
# contents the test has no business asserting.
if(D2D_FW_STUB_UNIT)
  target_compile_definitions(app PRIVATE
                             D2D_FW_STUB_UNIT="${D2D_FW_STUB_UNIT}"
                             D2D_FW_STUB_REPEAT=${D2D_FW_STUB_REPEAT})
endif()

# chip_init.c calls the drop's basic_init library rather than restating the
# reset and firewall sequence.
target_link_libraries(app PRIVATE gr_smc)
