# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_TT_FW_COMMON_EMUL reg_stub.c)
