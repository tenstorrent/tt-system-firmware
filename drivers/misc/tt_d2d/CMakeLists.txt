# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_TT_D2D tt_d2d.c)

if(CONFIG_TT_D2D_FW_BLOB)
  # An application may set TT_D2D_FW_BLOB_FILE to a path Kconfig cannot name,
  # such as one under its own build tree. Otherwise take the Kconfig value,
  # resolving a relative path against the module root where the drop is
  # unpacked.
  if(NOT DEFINED TT_D2D_FW_BLOB_FILE)
    set(TT_D2D_FW_BLOB_FILE "${CONFIG_TT_D2D_FW_BLOB_PATH}")
  endif()

  if(NOT IS_ABSOLUTE "${TT_D2D_FW_BLOB_FILE}")
    set(TT_D2D_FW_BLOB_FILE
        "${ZEPHYR_TT_SYSTEM_FIRMWARE_MODULE_DIR}/${TT_D2D_FW_BLOB_FILE}")
  endif()

  if(NOT EXISTS "${TT_D2D_FW_BLOB_FILE}")
    message(FATAL_ERROR
            "CONFIG_TT_D2D_FW_BLOB is set but there is no D2D firmware image at\n"
            "  ${TT_D2D_FW_BLOB_FILE}\n"
            "The image ships in the Sival tarball drop.")
  endif()

  # tt_d2d_load_fw() copies the image a word at a time and rejects a length
  # that is not a multiple of four. Catching that here names the offending file;
  # left to run time it is an -EINVAL from a call whose arguments look fine.
  file(SIZE "${TT_D2D_FW_BLOB_FILE}" tt_d2d_fw_blob_size)
  math(EXPR tt_d2d_fw_blob_tail "${tt_d2d_fw_blob_size} % 4")
  if(NOT tt_d2d_fw_blob_tail EQUAL 0)
    message(FATAL_ERROR
            "D2D firmware image is ${tt_d2d_fw_blob_size} bytes, which is not a "
            "multiple of 4 and cannot be loaded:\n  ${TT_D2D_FW_BLOB_FILE}")
  endif()

  zephyr_library_sources(d2d_fw_blob.S)
  zephyr_library_compile_definitions(TT_D2D_FW_BLOB_PATH="${TT_D2D_FW_BLOB_FILE}")

  # .incbin leaves no trace in the dependency scan, so without this a rebuild
  # after a new drop would keep the old image.
  set_property(SOURCE d2d_fw_blob.S APPEND PROPERTY
               OBJECT_DEPENDS "${TT_D2D_FW_BLOB_FILE}")
endif()
