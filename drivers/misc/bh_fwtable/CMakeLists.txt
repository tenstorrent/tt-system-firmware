# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(bh_fwtable)

# zephyr-keep-sorted-start
zephyr_library_sources_ifdef(CONFIG_BH_FWTABLE   bh_fwtable.c)
# zephyr-keep-sorted-stop

if(CONFIG_BH_FWTABLE)

include(${ZEPHYR_BASE}/modules/nanopb/nanopb.cmake)
zephyr_include_directories(
    ${NANOPB_INCLUDE_DIRS}
    ${CMAKE_BINARY_DIR}/modules/tt-system-firmware/drivers/misc/bh_fwtable/spirom_protobufs
)
zephyr_library_link_libraries(nanopb)

zephyr_nanopb_sources(bh_fwtable
    spirom_protobufs/flash_info.proto
    spirom_protobufs/fw_table.proto
    spirom_protobufs/fw_table_override.proto
    spirom_protobufs/read_only.proto
)

endif()
