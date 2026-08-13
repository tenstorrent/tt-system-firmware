# SPDX-License-Identifier: Apache-2.0

zephyr_library()
# Shared with bh_arc and other consumers of ReadReg/WriteReg/SetPostCode.
zephyr_include_directories(.)

# SoC-owned status_reg.h (Blackhole today; Grendel can add its own map later).
zephyr_library_include_directories_ifdef(CONFIG_TT_POST_CODE
  ${ZEPHYR_CURRENT_MODULE_DIR}/soc/tenstorrent/tt_blackhole/include)

zephyr_library_sources(post_code.c)
zephyr_library_sources_ifdef(CONFIG_TT_MSGQUEUE msgqueue.c)
zephyr_library_sources_ifdef(CONFIG_TT_FW_COMMON_EMUL reg_stub.c)

# Handler registrations (REGISTER_MESSAGE) live in product libs; keep the
# iterable section available even when the queue implementation is disabled.
zephyr_linker_sources(DATA_SECTIONS iterables.ld)
