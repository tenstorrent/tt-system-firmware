# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tt_grendel_filter)

set(SIVAL_EXTDIR ${CMAKE_BINARY_DIR}/grendel-sival-sdk)
set(SIVAL_BLOBS_TARBALL ${CMAKE_CURRENT_SOURCE_DIR}/../../../../zephyr/blobs/grendel-sival-sdk.tar.gz)
set(SIVAL_SDK ${SIVAL_EXTDIR}/grendel_sival_sdk)
set(SYSFW_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)

file(MAKE_DIRECTORY ${SIVAL_EXTDIR})
execute_process(COMMAND tar -xzf ${SIVAL_BLOBS_TARBALL} WORKING_DIRECTORY ${SIVAL_EXTDIR})

target_compile_definitions(app PRIVATE PLATFORM_KER_SMC=1)

# Stub regs.h must win over SiVal regs.h.
target_include_directories(app BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(app PRIVATE
  ${SIVAL_EXTDIR}
  ${SIVAL_SDK}/common/include
  ${SIVAL_SDK}/common/include/registers
  ${SIVAL_SDK}/common/include/registers/common
  ${SIVAL_SDK}/common/include/registers/ker
  ${SIVAL_SDK}/firmware/common/include
  ${SIVAL_SDK}/firmware/drivers/firewall/include
)

target_sources(app PRIVATE
  src/main.c
  src/reg_stub.c
  src/sival_addrs.c
  ${SYSFW_ROOT}/drivers/misc/tt_grendel_filter/tt_grendel_filter.c
  ${SIVAL_SDK}/firmware/drivers/firewall/src/firewall.c
)

set(TT_FORCE_REG_STUB
  src/sival_addrs.c
  ${SYSFW_ROOT}/drivers/misc/tt_grendel_filter/tt_grendel_filter.c
  ${SIVAL_SDK}/firmware/drivers/firewall/src/firewall.c
)
set_property(SOURCE ${TT_FORCE_REG_STUB} APPEND PROPERTY COMPILE_OPTIONS
  -include ${CMAKE_CURRENT_SOURCE_DIR}/src/regs.h
)
