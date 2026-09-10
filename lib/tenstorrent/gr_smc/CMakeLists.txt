# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(gr_smc)

zephyr_library_sources(grendel_utils.c)

zephyr_library_sources_ifdef(CONFIG_TT_GR_SMC_LOG_BRIDGE grendel_log_bridge.c)

# Sival SDK compilations
set(SIVAL_SDK ${CMAKE_BINARY_DIR}/grendel-sival-sdk/grendel_sival_sdk)

zephyr_library_sources(
  ${SIVAL_SDK}/firmware/drivers/firewall/src/firewall.c
  ${SIVAL_SDK}/firmware/drivers/noc/src/itn.c
  ${SIVAL_SDK}/firmware/drivers/smc/src/smc_init.c
  ${SIVAL_SDK}/firmware/libs/basic_init/src/basic_init.c
  ${SIVAL_SDK}/firmware/libs/noc/src/noc_init.c)

zephyr_library_sources_ifdef(CONFIG_TT_GR_SMC_ERR_STR ${SIVAL_SDK}/firmware/common/src/err.c)
