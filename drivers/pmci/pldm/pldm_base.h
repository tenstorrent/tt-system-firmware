/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PMCI_PLDM_BASE_H_
#define PMCI_PLDM_BASE_H_

#include <zephyr/device.h>
#include <libpldm/base.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Encode a completion-code-only PLDM response.
 *
 * Shared utility used by both BASE and PLATFORM response handlers.
 */
int pldm_cc_only_response(const struct pldm_header_info *req_hdr, uint8_t cc,
			  struct pldm_msg *resp_msg, size_t *resp_pldm_len);

/**
 * @brief Build a response for a PLDM_BASE typed request.
 *
 * @param tid             Pointer to the local terminus ID. GetTID reads it;
 *                        SetTID writes a new value to it.
 * @param mctp_dev        The tenstorrent,pldm-mctp-responder device instance.
 *                        Used to obtain the OEM handler (if any) for
 *                        GetPLDMVersion / GetPLDMCommands / GetPLDMTypes.
 * @param req_hdr         Decoded request header.
 * @param req_msg         Raw request message (including header).
 * @param req_payload_len Payload length (bytes after the PLDM header).
 * @param resp_msg        Output buffer for the response.
 * @param resp_pldm_len   Set to the response length on success.
 * @return 0 on success, negative errno on failure.
 */
int pldm_base_build_response(uint8_t *tid, const struct device *mctp_dev,
			     const struct pldm_header_info *req_hdr, const struct pldm_msg *req_msg,
			     size_t req_payload_len, struct pldm_msg *resp_msg,
			     size_t *resp_pldm_len);

#endif /* PMCI_PLDM_BASE_H_ */
