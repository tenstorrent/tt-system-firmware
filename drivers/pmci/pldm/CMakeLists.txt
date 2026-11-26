# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_PMCI_PLDM_PLATFORM pldm_platform.c)
zephyr_library_sources_ifdef(CONFIG_TT_PMCI_PLDM_MCTP_RESPONDER pldm_mctp_responder.c)
zephyr_library_sources_ifdef(CONFIG_PMCI_PLDM_BASE pldm_base.c)
zephyr_library_sources_ifdef(CONFIG_PMCI_PLDM_PLATFORM pldm_pdr.c)
zephyr_library_sources_ifdef(CONFIG_TT_PMCI_PLDM_SHELL tt_pldm_oem.c)

zephyr_syscall_header(
  ${ZEPHYR_TT_SYSTEM_FIRMWARE_MODULE_DIR}/include/zephyr/drivers/pmci/pldm/pldm_oem_handler.h)

# libpldm's Zephyr module does not compile its firmware device responder.
# Build it here, with the ABI visibility macros its own build would define.
# Keep this block last: zephyr_library_named() makes tt_libpldm_fd the current
# library, so sources added after it would be built with those macros.
if(CONFIG_TT_PMCI_PLDM_LIBPLDM_FD)
  zephyr_library_named(tt_libpldm_fd)
  zephyr_library_sources(
    ${ZEPHYR_LIBPLDM_MODULE_DIR}/src/control.c
    libpldm_fd.c
  )
  zephyr_library_compile_definitions(
    LIBPLDM_ABI_STABLE=
    LIBPLDM_ABI_TESTING=
    LIBPLDM_ABI_DEPRECATED=
    LIBPLDM_ABI_DEPRECATED_UNSAFE=
    _GNU_SOURCE
  )
endif()
