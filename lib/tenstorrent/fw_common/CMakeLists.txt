# SPDX-License-Identifier: Apache-2.0

zephyr_library()
# Shared with bh_arc and other consumers of ReadReg/WriteReg.
zephyr_include_directories(.)

zephyr_library_sources_ifdef(CONFIG_TT_FW_COMMON_EMUL reg_stub.c)
