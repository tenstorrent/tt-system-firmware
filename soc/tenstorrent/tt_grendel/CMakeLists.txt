# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(.)

add_subdirectory(${SOC_NAME})
add_subdirectory_ifdef(CONFIG_TT_GRENDEL_SMC tt_grendel_smc)
add_subdirectory_ifdef(CONFIG_TT_GRENDEL_SEP tt_grendel_sep)
