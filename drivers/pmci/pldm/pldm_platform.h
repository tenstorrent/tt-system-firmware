/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PMCI_PLDM_PLATFORM_H_
#define PMCI_PLDM_PLATFORM_H_

#include "pldm_pdr.h"

#include <libpldm/base.h>

#include <stddef.h>

const ver32_t *pldm_platform_versions_get(size_t *versions_size);

const bitfield8_t *pldm_platform_commands_get(void);

int pldm_platform_build_response(const struct device *pdr_dev,
				 const struct pldm_header_info *req_hdr,
				 const struct pldm_msg *req_msg, size_t req_payload_len,
				 struct pldm_msg *resp_msg, size_t *resp_pldm_len);

#endif /* PMCI_PLDM_PLATFORM_H_ */
