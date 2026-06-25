# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_TT_BH_EFUSE bh_efuse.c)
zephyr_library_sources_ifdef(CONFIG_TT_BH_EFUSE_EMUL bh_efuse_emul.c)

zephyr_syscall_header_ifdef(CONFIG_TT_BH_EFUSE
  ${ZEPHYR_TT_SYSTEM_FIRMWARE_MODULE_DIR}/include/zephyr/drivers/misc/bh_efuse.h)
