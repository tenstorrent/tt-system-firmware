# SPDX-License-Identifier: Apache-2.0

if(NOT CONFIG_BOARD_TT_BLACKHOLE_TT_BLACKHOLE_SMC)
  return()
endif()

zephyr_library()
if(CONFIG_MSPI AND CONFIG_FLASH)
  zephyr_library_sources(src/flash.c)
endif()
zephyr_library_sources_ifdef(CONFIG_UART_TT_VIRT src/vuart.c)
zephyr_library_include_directories(${ZEPHYR_TT_SYSTEM_FIRMWARE_MODULE_DIR}/lib/tenstorrent/bh_arc)

#
# Encode firmware table files via protobufs
#
# include(${ZEPHYR_BASE}/modules/nanopb/nanopb.cmake)
set(PROTOC ${ZEPHYR_NANOPB_MODULE_DIR}/generator/protoc)
zephyr_library_include_directories(${CMAKE_BINARY_DIR}/modules/tt-system-firmware/drivers/misc/bh_fwtable)

set(MOD_DIR ${ZEPHYR_TT_SYSTEM_FIRMWARE_MODULE_DIR})
set(SPIROM_PROTOBUFS ${MOD_DIR}/drivers/misc/bh_fwtable/spirom_protobufs)

message(STATUS "Generating Python files from protobufs")
set(OUTPUT_DIR ${ZEPHYR_BINARY_DIR}/python_proto_files)
file(MAKE_DIRECTORY ${OUTPUT_DIR})
set_property(GLOBAL APPEND PROPERTY extra_post_build_commands
  COMMAND ${PROTOC} --python_out=${OUTPUT_DIR} ${SPIROM_PROTOBUFS}/fw_table.proto -I ${SPIROM_PROTOBUFS}
  COMMAND ${PROTOC} --python_out=${OUTPUT_DIR} ${SPIROM_PROTOBUFS}/fw_table_override.proto -I ${SPIROM_PROTOBUFS}
  COMMAND ${PROTOC} --python_out=${OUTPUT_DIR} ${SPIROM_PROTOBUFS}/read_only.proto -I ${SPIROM_PROTOBUFS}
  COMMAND ${PROTOC} --python_out=${OUTPUT_DIR} ${SPIROM_PROTOBUFS}/flash_info.proto -I ${SPIROM_PROTOBUFS}
)

if(BOARD STREQUAL "tt_blackhole")
  # Map board revision names to folder name
  string(TOUPPER ${BOARD_REVISION} PROD_NAME)
elseif(BOARD STREQUAL "native_sim")
  # Use P100 data files to stand in
  set(PROD_NAME "P100")
else()
  message(FATAL_ERROR "No support for board ${BOARD}")
endif()

message(STATUS "Generating rom configuration binary files for ${PROD_NAME}")

include(${MOD_DIR}/cmake/bundle_version.cmake)

# variable is needed to avoid protobuf version clashing
if(PROD_NAME MATCHES "^P300")
  set_property(GLOBAL APPEND PROPERTY extra_post_build_commands
    COMMAND PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python
      ${PYTHON_EXECUTABLE} ${MOD_DIR}/scripts/encode_spirom_bins.py
      --board ${PROD_NAME}_L --build-dir ${CMAKE_BINARY_DIR} --output ${CMAKE_BINARY_DIR}/generated_board_cfg
      --bundle-version ${BUNDLE_VERSION_STRING}
  )
  set_property(GLOBAL APPEND PROPERTY extra_post_build_commands
    COMMAND PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python
      ${PYTHON_EXECUTABLE} ${MOD_DIR}/scripts/encode_spirom_bins.py
      --board ${PROD_NAME}_R --build-dir ${CMAKE_BINARY_DIR} --output ${CMAKE_BINARY_DIR}/generated_board_cfg
      --bundle-version ${BUNDLE_VERSION_STRING}
  )
else()
  set_property(GLOBAL APPEND PROPERTY extra_post_build_commands
    COMMAND PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python
      ${PYTHON_EXECUTABLE} ${MOD_DIR}/scripts/encode_spirom_bins.py
      --board ${PROD_NAME} --build-dir ${CMAKE_BINARY_DIR} --output ${CMAKE_BINARY_DIR}/generated_board_cfg
      --bundle-version ${BUNDLE_VERSION_STRING}
  )
endif()
