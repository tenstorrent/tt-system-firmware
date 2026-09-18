# SPDX-License-Identifier: Apache-2.0
set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/riscv/common/linker.ld CACHE INTERNAL "")
zephyr_include_directories(.)

# SiVal registers/platform.h requires a platform select (ker_smc).
zephyr_compile_definitions(PLATFORM_KER_SMC=1)

# The trampoline has to be the first instruction in the image, which means
# going into rom_start ahead of __start. rom_start is assembled from snippets
# ordered by sort key, and the two that matter are Zephyr's own: the image is
# padded to CONFIG_ROM_START_OFFSET under "0x0", and __start is placed under
# "0x0vectors". Landing between them means a key that sorts after the first and
# before the second; ahead of the padding is not an option, as it rewinds the
# location counter over anything already emitted.
zephyr_library()
zephyr_library_sources(boot_trampoline.S)
zephyr_linker_sources(ROM_START SORT_KEY 0x0boot boot_trampoline.ld)

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
zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/common/include/)
zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/common/include/)
zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/drivers/firewall/include)
zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/drivers/addr_remap/include)
zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/drivers/noc/include)
zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/drivers/smc/include)
zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/chiplet_config)
zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/libs/basic_init/include)
zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/libs/noc/include)
zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/libs/clock/include)
zephyr_include_directories(${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/libs/hsio_tile/include)
