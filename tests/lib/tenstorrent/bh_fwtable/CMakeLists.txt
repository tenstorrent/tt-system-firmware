# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bh_fwtable_sim)

target_sources(app PRIVATE src/main.c)

# ───── Paths to inputs and tools ─────────────────────────────────────────
set(TTSF_ROOT     "${ZEPHYR_BASE}/../tt-system-firmware")
set(PROTO_DIR     "${TTSF_ROOT}/drivers/misc/bh_fwtable/spirom_protobufs")
set(TXT_DIR       "${TTSF_ROOT}/boards/tenstorrent/tt_blackhole/spirom_data_tables/P150B")
set(ENC_SCRIPT    "${TTSF_ROOT}/scripts/encode_spirom_bins.py")
set(MKFS_SCRIPT   "${TTSF_ROOT}/scripts/tt_boot_fs.py")
set(FLASH_SPEC    "${CMAKE_CURRENT_SOURCE_DIR}/flash_spec.yaml")

set(PROTO_PY_DIR  "${CMAKE_BINARY_DIR}/zephyr/python_proto_files")
set(BIN_DIR       "${CMAKE_BINARY_DIR}/smc/generated_board_cfg/P150B")
set(FLASH_BIN     "${CMAKE_BINARY_DIR}/flash.bin")

# Need protoc on PATH (apt: protobuf-compiler) for the .proto → *_pb2.py step.
find_program(PROTOC_EXE protoc)
if(NOT PROTOC_EXE)
    message(FATAL_ERROR
        "protoc not found. Install with 'sudo apt install protobuf-compiler'. "
        "It is required to generate flash.bin for these tests.")
endif()

if(PROTOC_EXE)
    # ───── Step 1: .proto → *_pb2.py (protoc) ────────────────────────────
    set(PB_PY_FILES
        "${PROTO_PY_DIR}/read_only_pb2.py"
        "${PROTO_PY_DIR}/fw_table_pb2.py"
        "${PROTO_PY_DIR}/flash_info_pb2.py"
    )
    add_custom_command(
        OUTPUT  ${PB_PY_FILES}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PROTO_PY_DIR}"
        COMMAND ${PROTOC_EXE}
                --python_out=${PROTO_PY_DIR}
                -I ${PROTO_DIR}
                ${PROTO_DIR}/read_only.proto
                ${PROTO_DIR}/fw_table.proto
                ${PROTO_DIR}/flash_info.proto
        DEPENDS ${PROTO_DIR}/read_only.proto
                ${PROTO_DIR}/fw_table.proto
                ${PROTO_DIR}/flash_info.proto
        COMMENT "Generating Python protobuf modules from .proto schemas"
        VERBATIM
    )

    # ───── Step 2: .txt + *_pb2.py → *.bin (encode_spirom_bins.py) ───────
    set(PB_BIN_FILES
        "${BIN_DIR}/read_only.bin"
        "${BIN_DIR}/fw_table.bin"
        "${BIN_DIR}/flash_info.bin"
    )
    add_custom_command(
        OUTPUT  ${PB_BIN_FILES}
        COMMAND ${CMAKE_COMMAND} -E env PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python
                ${PYTHON_EXECUTABLE} ${ENC_SCRIPT}
                --board P150B
                --build-dir ${CMAKE_BINARY_DIR}
                --output ${CMAKE_BINARY_DIR}/smc/generated_board_cfg
                --bundle-version 0.0.0.0
        DEPENDS ${PB_PY_FILES}
                ${TXT_DIR}/read_only.txt
                ${TXT_DIR}/fw_table.txt
                ${TXT_DIR}/flash_info.txt
        COMMENT "Encoding P150B protobuf .bin payloads"
        VERBATIM
    )

    # ───── Step 3: *.bin + spec → flash.bin (tt_boot_fs.py mkfs) ─────────
    add_custom_command(
        OUTPUT  ${FLASH_BIN}
        COMMAND ${PYTHON_EXECUTABLE} ${MKFS_SCRIPT} mkfs
                --build-dir ${CMAKE_BINARY_DIR}
                ${FLASH_SPEC}
                ${FLASH_BIN}
        DEPENDS ${FLASH_SPEC} ${PB_BIN_FILES}
        COMMENT "Generating ${FLASH_BIN} from ${FLASH_SPEC}"
        VERBATIM
    )

    add_custom_target(generate_flash_bin DEPENDS ${FLASH_BIN})
    add_dependencies(app generate_flash_bin)
endif()
