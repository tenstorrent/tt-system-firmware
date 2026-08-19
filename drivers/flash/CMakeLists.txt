# SPDX-License-Identifier: Apache-2.0

zephyr_library_amend()

zephyr_library_include_directories("${ZEPHYR_BASE}/drivers/flash")

# zephyr-keep-sorted-start
zephyr_library_sources_ifdef(CONFIG_FLASH_TT_MUX flash_mux.c)
zephyr_library_sources_ifdef(CONFIG_FLASH_TT_STM32_WRAPPER flash_stm32_wrapper.c)
# zephyr-keep-sorted-stop
