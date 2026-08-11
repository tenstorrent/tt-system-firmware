# SPDX-License-Identifier: Apache-2.0

zephyr_library()
# Shared with bh_arc and other consumers of ReadReg/WriteReg/SetPostCode.
zephyr_include_directories(.)

# TT_POST_CODE uses the Blackhole status_reg map (STATUS_POST_CODE_REG_ADDR).
zephyr_library_include_directories_ifdef(CONFIG_TT_POST_CODE
                                         ${CMAKE_CURRENT_SOURCE_DIR}/../bh_arc)

zephyr_library_sources(post_code.c)
zephyr_library_sources_ifdef(CONFIG_TT_MSGQUEUE msgqueue.c)
zephyr_library_sources_ifdef(CONFIG_TT_FW_COMMON_EMUL reg_stub.c)

# Handler registrations (REGISTER_MESSAGE) live in product libs; keep the
# iterable section available even when the queue implementation is disabled.
zephyr_linker_sources(DATA_SECTIONS iterables.ld)
