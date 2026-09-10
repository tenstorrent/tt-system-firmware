# SPDX-License-Identifier: Apache-2.0
set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/riscv/common/linker.ld CACHE INTERNAL "")
zephyr_include_directories(.)

# Add SOC setup
zephyr_compile_definitions(PLATFORM_MIMIR_SMC=1)

if(CONFIG_SIVAL_SHIM)
  set(SIVAL_EXTDIR ${CMAKE_BINARY_DIR}/grendel-sival-sdk)
  set(SIVAL_BLOBS_TARBALL ${CMAKE_CURRENT_SOURCE_DIR}/../../../../zephyr/blobs/grendel-sival-sdk.tar.gz)

  file(MAKE_DIRECTORY ${SIVAL_EXTDIR})
  execute_process(COMMAND tar -xzf ${SIVAL_BLOBS_TARBALL}
    WORKING_DIRECTORY ${SIVAL_EXTDIR}
  )
  zephyr_include_directories(${SIVAL_EXTDIR})
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/common/include/registers)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/common/include/registers/common)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/common/include/registers/mimir)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/common/include/registers/mimir/smc_view)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/common/include/)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/common/include/)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/drivers/firewall/include)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/drivers/addr_remap/include)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/drivers/noc/include)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/drivers/smc/include)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/drivers/gddr/include)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/chiplet_config)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/libs/basic_init/include)
  zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/libs/noc/include)
else()
  set(SIVAL_EXTDIR ${CMAKE_BINARY_DIR}/grendel-sival-sources)
  set(SIVAL_TARBALL ${CMAKE_CURRENT_SOURCE_DIR}/../../../../zephyr/blobs/grendel-sival-sources.tar.gz)

  file(MAKE_DIRECTORY ${SIVAL_EXTDIR})
  execute_process(COMMAND tar -xzf ${SIVAL_TARBALL}
    WORKING_DIRECTORY ${SIVAL_EXTDIR}
  )
  zephyr_include_directories(${SIVAL_EXTDIR})

endif()
