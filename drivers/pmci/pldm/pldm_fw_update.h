/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TT_PMCI_PLDM_FW_UPDATE_H_
#define TT_PMCI_PLDM_FW_UPDATE_H_

#include <stddef.h>
#include <stdint.h>

#include <libpldm/base.h>

/** PLDM for Firmware Update (DSP0267) 1.1.0, then CRC32 over the version entries. */
extern const ver32_t tt_pldm_fw_update_versions[2];

/** Commands the firmware device answers, as a GetPLDMCommands bitmap. */
extern const bitfield8_t tt_pldm_fw_update_commands[32];

/**
 * @brief Initialise the DSP0267 firmware device.
 *
 * Safe to call more than once.
 *
 * @return 0 on success, negative errno on failure.
 */
int tt_pldm_fw_update_init(void);

/**
 * @brief Handle a PLDM type 5 message (request or response).
 *
 * @param eid      Source endpoint of the message.
 * @param msg      PLDM message, starting at the PLDM header.
 * @param len      Length of @p msg.
 * @param resp     Buffer for a response to @p msg.
 * @param resp_len In: size of @p resp. Out: length of the response, or 0 when
 *                 nothing is to be sent (for example when @p msg is a response).
 * @return 0 on success, negative errno on failure.
 */
int tt_pldm_fw_update_handle_msg(uint8_t eid, const void *msg, size_t len, void *resp,
				 size_t *resp_len);

/**
 * @brief Advance an update: send the next request or a retry, apply timeouts.
 *
 * Call after every message handled by tt_pldm_fw_update_handle_msg(), and
 * periodically while idle so that retries and timeouts run.
 *
 * @param req     Buffer for a request to the update agent.
 * @param req_len In: size of @p req. Out: length of the request, or 0 when
 *                nothing is to be sent.
 * @param eid     Out: endpoint to send the request to.
 * @return 0 when progress ran; negative errno otherwise, including -EINVAL from libpldm
 *         when no update is in a transfer, verify or apply phase. Send only when 0 is
 *         returned and @p req_len is not 0.
 */
int tt_pldm_fw_update_progress(void *req, size_t *req_len, uint8_t *eid);

#endif /* TT_PMCI_PLDM_FW_UPDATE_H_ */
