# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_PMCI_PLDM_PLATFORM pldm_pdr.c)
