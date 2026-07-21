/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/util.h>

#include <libpldm/base.h>

#include <string.h>

#include "pldm_base.h"
#include "pldm_pdr.h"
#ifdef CONFIG_PMCI_PLDM_PLATFORM
#include "pldm_platform.h"
#endif

#ifdef CONFIG_PMCI_PLDM_OEM
#include "pldm_mctp_responder.h"
#include <zephyr/drivers/pmci/pldm/pldm_oem_handler.h>
#endif

int pldm_cc_only_response(const struct pldm_header_info *req_hdr, uint8_t cc,
			  struct pldm_msg *resp_msg, size_t *resp_pldm_len)
{
	cc = encode_cc_only_resp(req_hdr->instance, req_hdr->pldm_type, req_hdr->command, cc,
				 resp_msg);
	if (cc != PLDM_SUCCESS) {
		return -EINVAL;
	}

	*resp_pldm_len = sizeof(struct pldm_msg_hdr) + 1U;
	return 0;
}

static int pldm_handle_get_tid(const struct pldm_header_info *req_hdr, const uint8_t *tid,
			       size_t req_payload_len, struct pldm_msg *resp_msg,
			       size_t *resp_pldm_len)
{
	uint8_t cc;

	if (req_payload_len != PLDM_GET_TID_REQ_BYTES) {
		return pldm_cc_only_response(req_hdr, PLDM_ERROR_INVALID_LENGTH, resp_msg,
					     resp_pldm_len);
	}

	cc = encode_get_tid_resp(req_hdr->instance, PLDM_SUCCESS, *tid, resp_msg);
	if (cc != PLDM_SUCCESS) {
		return -EINVAL;
	}

	*resp_pldm_len = sizeof(struct pldm_msg_hdr) + PLDM_GET_TID_RESP_BYTES;
	return 0;
}

static int pldm_handle_set_tid(const struct pldm_header_info *req_hdr, uint8_t *tid,
			       const struct pldm_msg *req_msg, size_t req_payload_len,
			       struct pldm_msg *resp_msg, size_t *resp_pldm_len)
{
	uint8_t new_tid;
	int rc;

	if (req_payload_len != PLDM_SET_TID_REQ_BYTES) {
		return pldm_cc_only_response(req_hdr, PLDM_ERROR_INVALID_LENGTH, resp_msg,
					     resp_pldm_len);
	}

	rc = decode_set_tid_req(req_msg, req_payload_len, &new_tid);
	if (rc != PLDM_SUCCESS) {
		return pldm_cc_only_response(req_hdr, PLDM_ERROR_INVALID_DATA, resp_msg,
					     resp_pldm_len);
	}

	/* TID 0x00 and 0xFF are reserved per DSP0240 */
	if (new_tid == 0x00U || new_tid == 0xFFU) {
		return pldm_cc_only_response(req_hdr, PLDM_ERROR_INVALID_DATA, resp_msg,
					     resp_pldm_len);
	}

	*tid = new_tid;

	return pldm_cc_only_response(req_hdr, PLDM_SUCCESS, resp_msg, resp_pldm_len);
}

static int pldm_handle_get_types(const struct pldm_header_info *req_hdr,
				 const struct device *oem_handler, size_t req_payload_len,
				 struct pldm_msg *resp_msg, size_t *resp_pldm_len)
{
	struct pldm_base_get_pldm_types_resp response = {0};
	size_t payload_len = PLDM_BASE_GET_PLDM_TYPES_RESP_BYTES;
	int rc;

	if (req_payload_len != PLDM_GET_TYPES_REQ_BYTES) {
		return pldm_cc_only_response(req_hdr, PLDM_ERROR_INVALID_LENGTH, resp_msg,
					     resp_pldm_len);
	}

	response.completion_code = PLDM_SUCCESS;
	response.pldm_types[PLDM_BASE / 8].byte |= BIT(PLDM_BASE % 8);

#if CONFIG_PMCI_PLDM_PLATFORM
	response.pldm_types[PLDM_PLATFORM / 8].byte |= BIT(PLDM_PLATFORM % 8);
#endif

#if CONFIG_PMCI_PLDM_OEM
	if (oem_handler != NULL) {
		response.pldm_types[PLDM_OEM_TYPE / 8].byte |= BIT(PLDM_OEM_TYPE % 8);
	}
#endif

	rc = encode_pldm_base_get_pldm_types_resp(req_hdr->instance, &response, resp_msg,
						  &payload_len);
	if (rc != 0) {
		return -EINVAL;
	}

	*resp_pldm_len = sizeof(struct pldm_msg_hdr) + payload_len;
	return 0;
}

static int pldm_handle_get_version(const struct pldm_header_info *req_hdr,
				   const struct device *oem_handler, const struct pldm_msg *req_msg,
				   size_t req_payload_len, struct pldm_msg *resp_msg,
				   size_t *resp_pldm_len)
{
	static const ver32_t base_versions[] = {
		/* PLDM 1.1.0 followed by CRC32 over version entries. */
		{.alpha = 0x00, .update = 0xf0, .minor = 0xf1, .major = 0xf1},
		{.alpha = 0xba, .update = 0xbe, .minor = 0x9d, .major = 0x53},
	};
	uint32_t transfer_handle;
	uint8_t transfer_opflag;
	uint8_t type;
	uint8_t cc;
	const ver32_t *versions;
	size_t versions_size;

	cc = decode_get_version_req(req_msg, req_payload_len, &transfer_handle, &transfer_opflag,
				    &type);
	if (cc != PLDM_SUCCESS) {
		return pldm_cc_only_response(req_hdr, cc, resp_msg, resp_pldm_len);
	}

	ARG_UNUSED(transfer_handle);

	if (transfer_opflag != PLDM_GET_FIRSTPART) {
		return pldm_cc_only_response(req_hdr,
					     PLDM_GET_PLDM_VERSION_INVALID_TRANSFER_OPERATION_FLAG,
					     resp_msg, resp_pldm_len);
	}

	if (type == PLDM_BASE) {
		versions = base_versions;
		versions_size = sizeof(base_versions);
	}
#if CONFIG_PMCI_PLDM_PLATFORM
	else if (type == PLDM_PLATFORM) {
		versions = pldm_platform_versions_get(&versions_size);
		if (versions == NULL || versions_size == 0U) {
			return pldm_cc_only_response(req_hdr, PLDM_ERROR_UNSUPPORTED_PLDM_CMD,
						     resp_msg, resp_pldm_len);
		}
	}
#endif /*CONFIG_PMCI_PLDM_PLATFORM*/
#if CONFIG_PMCI_PLDM_OEM
	else if (type == PLDM_OEM_TYPE && oem_handler != NULL) {
		versions = pldm_oem_handler_get_versions(oem_handler, &versions_size);
		if (versions == NULL || versions_size == 0U) {
			return pldm_cc_only_response(req_hdr, PLDM_ERROR_UNSUPPORTED_PLDM_CMD,
						     resp_msg, resp_pldm_len);
		}
	}
#endif
	else {
		return pldm_cc_only_response(
			req_hdr, PLDM_GET_PLDM_VERSION_INVALID_PLDM_TYPE_IN_REQUEST_DATA, resp_msg,
			resp_pldm_len);
	}

	cc = encode_get_version_resp(req_hdr->instance, PLDM_SUCCESS, 0U, PLDM_START_AND_END,
				     versions, versions_size, resp_msg);
	if (cc != PLDM_SUCCESS) {
		return -EINVAL;
	}

	*resp_pldm_len = sizeof(struct pldm_msg_hdr) + 1U + sizeof(uint32_t) + 1U + versions_size;
	return 0;
}

static int pldm_handle_get_commands(const struct pldm_header_info *req_hdr,
				    const struct device *oem_handler,
				    const struct pldm_msg *req_msg, size_t req_payload_len,
				    struct pldm_msg *resp_msg, size_t *resp_pldm_len)
{
	static const bitfield8_t base_commands[32] = {
		[0] = {.byte = (BIT(PLDM_GET_TID) | BIT(PLDM_SET_TID) | BIT(PLDM_GET_PLDM_VERSION) |
				BIT(PLDM_GET_PLDM_TYPES) | BIT(PLDM_GET_PLDM_COMMANDS))}};
	uint8_t type;
	ver32_t version;
	uint8_t cc;
	const bitfield8_t *commands;

	cc = decode_get_commands_req(req_msg, req_payload_len, &type, &version);
	if (cc != PLDM_SUCCESS) {
		return pldm_cc_only_response(req_hdr, cc, resp_msg, resp_pldm_len);
	}

	ARG_UNUSED(version);

	if (type == PLDM_BASE) {
		commands = base_commands;
	}
#if CONFIG_PMCI_PLDM_PLATFORM
	else if (type == PLDM_PLATFORM) {
		commands = pldm_platform_commands_get();
		if (commands == NULL) {
			return pldm_cc_only_response(req_hdr, PLDM_ERROR_UNSUPPORTED_PLDM_CMD,
						     resp_msg, resp_pldm_len);
		}
	}
#endif
#if CONFIG_PMCI_PLDM_OEM
	else if (type == PLDM_OEM_TYPE && oem_handler != NULL) {
		commands = pldm_oem_handler_get_commands(oem_handler);
		if (commands == NULL) {
			return pldm_cc_only_response(req_hdr, PLDM_ERROR_UNSUPPORTED_PLDM_CMD,
						     resp_msg, resp_pldm_len);
		}
	}
#endif
	else {
		return pldm_cc_only_response(
			req_hdr, PLDM_GET_PLDM_COMMANDS_INVALID_PLDM_TYPE_IN_REQUEST_DATA, resp_msg,
			resp_pldm_len);
	}

	cc = encode_get_commands_resp(req_hdr->instance, PLDM_SUCCESS, commands, resp_msg);
	if (cc != PLDM_SUCCESS) {
		return -EINVAL;
	}

	*resp_pldm_len = sizeof(struct pldm_msg_hdr) + PLDM_GET_COMMANDS_RESP_BYTES;
	return 0;
}

int pldm_base_build_response(uint8_t *tid, const struct device *mctp_dev,
			     const struct pldm_header_info *req_hdr, const struct pldm_msg *req_msg,
			     size_t req_payload_len, struct pldm_msg *resp_msg,
			     size_t *resp_pldm_len)
{
#ifndef CONFIG_PMCI_PLDM_OEM
	ARG_UNUSED(mctp_dev);
	const struct device *oem_handler = NULL;
#else
	const struct device *oem_handler = pldm_mctp_responder_oem_handler_get(mctp_dev);
#endif

	switch (req_hdr->command) {
	case PLDM_GET_TID:
		return pldm_handle_get_tid(req_hdr, tid, req_payload_len, resp_msg, resp_pldm_len);

	case PLDM_SET_TID:
		return pldm_handle_set_tid(req_hdr, tid, req_msg, req_payload_len, resp_msg,
					   resp_pldm_len);

	case PLDM_GET_PLDM_TYPES:
		return pldm_handle_get_types(req_hdr, oem_handler, req_payload_len, resp_msg,
					     resp_pldm_len);

	case PLDM_GET_PLDM_VERSION:
		return pldm_handle_get_version(req_hdr, oem_handler, req_msg, req_payload_len,
					       resp_msg, resp_pldm_len);

	case PLDM_GET_PLDM_COMMANDS:
		return pldm_handle_get_commands(req_hdr, oem_handler, req_msg, req_payload_len,
						resp_msg, resp_pldm_len);

	default:
		return pldm_cc_only_response(req_hdr, PLDM_ERROR_UNSUPPORTED_PLDM_CMD, resp_msg,
					     resp_pldm_len);
	}
}
