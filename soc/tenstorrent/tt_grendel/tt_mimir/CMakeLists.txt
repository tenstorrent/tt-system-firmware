# SPDX-License-Identifier: Apache-2.0
set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/riscv/common/linker.ld CACHE INTERNAL "")
zephyr_include_directories(.)

# Add SOC setup
zephyr_compile_definitions(PLATFORM_MIMIR_SMC=1)

set(SIVAL_EXTDIR ${CMAKE_BINARY_DIR}/grendel-sival-sources)
set(SIVAL_TARBALL ${CMAKE_CURRENT_SOURCE_DIR}/../../../../zephyr/blobs/grendel-sival-sources.tar.gz)

file(MAKE_DIRECTORY ${SIVAL_EXTDIR})
execute_process(COMMAND tar -xzf ${SIVAL_TARBALL}
  WORKING_DIRECTORY ${SIVAL_EXTDIR}
)
zephyr_include_directories(${SIVAL_EXTDIR})

set(SIVAL_SDK_EXTDIR ${CMAKE_BINARY_DIR}/grendel-sival-sdk)
set(SIVAL_SDK_TARBALL ${CMAKE_CURRENT_SOURCE_DIR}/../../../../zephyr/blobs/grendel-sival-sdk.tar.gz)

file(MAKE_DIRECTORY ${SIVAL_SDK_EXTDIR})
execute_process(COMMAND tar -xzf ${SIVAL_SDK_TARBALL}
  WORKING_DIRECTORY ${SIVAL_SDK_EXTDIR}
)
zephyr_include_directories(${SIVAL_SDK_EXTDIR}/grendel_sival_sdk/firmware/drivers/gddr/include)
zephyr_include_directories(${SIVAL_SDK_EXTDIR}/grendel_sival_sdk/firmware/common/include)
