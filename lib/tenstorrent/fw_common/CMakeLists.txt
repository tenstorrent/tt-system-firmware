# SPDX-License-Identifier: Apache-2.0

zephyr_library()
# Shared with bh_arc and other consumers of ReadReg/WriteReg.
zephyr_include_directories(.)

zephyr_library_sources_ifdef(CONFIG_TT_MSGQUEUE msgqueue.c)
zephyr_library_sources_ifdef(CONFIG_TT_FW_COMMON_EMUL reg_stub.c)
zephyr_library_sources_ifdef(CONFIG_TT_TELEMETRY telemetry.c)

if(EXISTS ${APPLICATION_SOURCE_DIR}/VERSION)
  zephyr_library_compile_definitions(HAS_APP_VERSION=1)
endif()

# Handler registrations (REGISTER_MESSAGE) live in product libs; keep the
# iterable section available even when the queue implementation is disabled.
zephyr_linker_sources(DATA_SECTIONS iterables.ld)
