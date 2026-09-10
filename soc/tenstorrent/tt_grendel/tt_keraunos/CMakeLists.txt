# SPDX-License-Identifier: Apache-2.0
zephyr_include_directories(.)

# Add SOC setup
if(CONFIG_SOC_TT_KERAUNOS_SEP)
  # SEP is an ARC EM4 core; SMC is RISC-V.
  set(SOC_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/../tt_grendel_sep/linker.ld
      CACHE INTERNAL "")
  # No small-data section: the SEP linker script has no .sdata/.sbss output.
  zephyr_compile_options(-mno-sdata)
  zephyr_compile_definitions(PLATFORM_KER_SEP=1)
else()
  set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/riscv/common/linker.ld CACHE INTERNAL "")
  zephyr_compile_definitions(PLATFORM_KER_SMC=1)
endif()

# SEP does not use the SiVal SDK headers.
if(NOT CONFIG_SOC_TT_KERAUNOS_SEP)
  set(SIVAL_EXTDIR ${CMAKE_BINARY_DIR}/grendel-sival-sdk)
  set(SIVAL_BLOBS_TARBALL ${CMAKE_CURRENT_SOURCE_DIR}/../../../../zephyr/blobs/grendel-sival-sdk.tar.gz)

  file(MAKE_DIRECTORY ${SIVAL_EXTDIR})
  execute_process(COMMAND tar -xzf ${SIVAL_BLOBS_TARBALL}
    WORKING_DIRECTORY ${SIVAL_EXTDIR}
  )
  zephyr_include_directories(${SIVAL_EXTDIR})
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/common/include/registers)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/common/include/registers/common)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/common/include/registers/ker)

  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/common/include/)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/drivers/firewall/include)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/drivers/addr_remap/include)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/libs/basic_init/include)
endif()
