/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Build libpldm's firmware device responder.
 *
 * fd.c asserts that struct pldm_fd is aligned to PLDM_ALIGNOF_PLDM_FD (8). On
 * i386 (32-bit native_sim) uint64_t struct members are only 4-byte aligned, so
 * the assertion fails there. The macro only sizes caller-allocated storage, and
 * callers in this tree allocate the real struct type, so state the i386 value.
 */

#include <libpldm/firmware_fd.h>

#if defined(__i386__)
#undef PLDM_ALIGNOF_PLDM_FD
#define PLDM_ALIGNOF_PLDM_FD 4
#endif

#include <firmware_device/fd.c>
