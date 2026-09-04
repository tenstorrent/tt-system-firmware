# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(gr_smc)

zephyr_library_sources(grendel_utils.c)

zephyr_library_sources_ifdef(CONFIG_TT_GR_SMC_LOG_BRIDGE grendel_log_bridge.c)

if(CONFIG_TT_GR_SMC_ERR_STR)
  set(SIVAL_SDK ${CMAKE_BINARY_DIR}/grendel-sival-sdk/grendel_sival_sdk)
  zephyr_library_sources(${SIVAL_SDK}/firmware/common/src/err.c)
  zephyr_library_include_directories(${SIVAL_SDK}/firmware/common/include)
endif()
