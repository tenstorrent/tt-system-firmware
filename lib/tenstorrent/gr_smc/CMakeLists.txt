# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(gr_smc)

zephyr_library_sources(grendel_utils.c)

zephyr_library_sources_ifdef(CONFIG_TT_GR_SMC_LOG_BRIDGE grendel_log_bridge.c)

# Sival SDK compilations
set(SIVAL_SDK ${CMAKE_BINARY_DIR}/grendel-sival-sdk/grendel_sival_sdk)

set(SIVAL_SDK_SOURCES
  ${SIVAL_SDK}/firmware/drivers/addr_remap/src/remapper.c
  ${SIVAL_SDK}/firmware/drivers/firewall/src/firewall.c
  ${SIVAL_SDK}/firmware/drivers/noc/src/itn.c
  ${SIVAL_SDK}/firmware/drivers/smc/src/smc_init.c
  ${SIVAL_SDK}/firmware/libs/basic_init/src/basic_init.c
  ${SIVAL_SDK}/firmware/libs/clock/src/clock.c
  ${SIVAL_SDK}/firmware/libs/hsio_tile/src/hsio_tile_init.c
  ${SIVAL_SDK}/firmware/libs/noc/src/noc_init.c)

zephyr_library_sources(${SIVAL_SDK_SOURCES})

# remapper.c switches over an enum without handling the terminating member,
# which id flagged by -Werror=switch
set_source_files_properties(${SIVAL_SDK_SOURCES}
  PROPERTIES COMPILE_OPTIONS "-Wno-error")

zephyr_library_include_directories(
  ${SIVAL_SDK}/common/include
  ${SIVAL_SDK}/firmware/common/include
  ${SIVAL_SDK}/firmware/libs/clock/include
  ${SIVAL_SDK}/firmware/libs/basic_init/include
  ${SIVAL_SDK}/firmware/libs/hsio_tile/include
  ${SIVAL_SDK}/firmware/drivers/firewall/include
  ${SIVAL_SDK}/firmware/drivers/smc/include)

zephyr_library_sources_ifdef(CONFIG_TT_GR_SMC_ERR_STR ${SIVAL_SDK}/firmware/common/src/err.c)
