/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/sys/crc.h>
#include <tenstorrent/occp.h>
#include "occp_private.h"

#define FAKE_MEM_SIZE 0x10000
static uint8_t fake_mem[FAKE_MEM_SIZE];

#define FAKE_RESP_BUF_SIZE (OCCP_MAX_MSG_SIZE + sizeof(struct occp_header) + 4)
static uint8_t fake_resp_buf[FAKE_RESP_BUF_SIZE];
static size_t fake_resp_len;

static void build_resp_header(uint8_t app_id, uint8_t msg_id, uint16_t body_len,
			      struct occp_header *hdr)
{
	memset(hdr, 0, sizeof(*hdr));
	hdr->cmd_header.app_id = app_id;
	hdr->cmd_header.msg_id = msg_id;
	hdr->cmd_header.length = body_len;
	hdr->header_crc = crc8((uint8_t *)hdr + 1, sizeof(*hdr) - 1, 0xD3, 0xFF, false);
}

static int fake_send(const struct occp_backend *backend, const uint8_t *data, size_t length)
{
	const struct occp_header *hdr = (const struct occp_header *)data;
	struct occp_header resp_hdr;
	size_t crc_len;

	if (length < sizeof(struct occp_header)) {
		return -EINVAL;
	}

	switch (hdr->cmd_header.msg_id) {
	case OCCP_BASE_MSG_GET_VERSION: {
		struct occp_get_version_response resp = {0};

		build_resp_header(OCCP_APP_BASE, OCCP_BASE_MSG_GET_VERSION,
				  sizeof(resp) - sizeof(resp.header), &resp.header);
		resp.major_version = 1;
		resp.minor_version = 0;
		resp.patch_version = 0;
		memcpy(fake_resp_buf, &resp, sizeof(resp));
		fake_resp_len = sizeof(resp);
		break;
	}
	case OCCP_BASE_MSG_WRITE_DATA: {
		const struct occp_write_data_request *req =
			(const struct occp_write_data_request *)data;
		uint64_t addr = ((uint64_t)req->address_high << 32) | req->address_low;
		size_t wlen = req->length;
		const uint8_t *payload = data + sizeof(*req);

		if (addr + wlen <= FAKE_MEM_SIZE) {
			memcpy(fake_mem + addr, payload, wlen);
		}
		build_resp_header(OCCP_APP_BASE, OCCP_BASE_MSG_WRITE_DATA, 0, &resp_hdr);
		memcpy(fake_resp_buf, &resp_hdr, sizeof(resp_hdr));
		fake_resp_len = sizeof(resp_hdr);
		break;
	}
	case OCCP_BASE_MSG_READ_DATA: {
		const struct occp_read_data_request *req =
			(const struct occp_read_data_request *)data;
		uint64_t addr = ((uint64_t)req->address_high << 32) | req->address_low;
		size_t rlen = req->length;

		crc_len = (rlen > 13) ? 4 : 1;
		build_resp_header(OCCP_APP_BASE, OCCP_BASE_MSG_READ_DATA, rlen, &resp_hdr);
		memcpy(fake_resp_buf, &resp_hdr, sizeof(resp_hdr));
		if (addr + rlen <= FAKE_MEM_SIZE) {
			memcpy(fake_resp_buf + sizeof(resp_hdr), fake_mem + addr, rlen);
		} else {
			memset(fake_resp_buf + sizeof(resp_hdr), 0, rlen);
		}
		memset(fake_resp_buf + sizeof(resp_hdr) + rlen, 0, crc_len);
		fake_resp_len = sizeof(resp_hdr) + rlen + crc_len;
		break;
	}
	default:
		return -ENOTSUP;
	}

	return 0;
}

static int fake_receive(const struct occp_backend *backend, uint8_t *data, size_t length)
{
	if (length > fake_resp_len) {
		return -ENOMEM;
	}
	memcpy(data, fake_resp_buf, length);
	return 0;
}

static struct occp_backend fake_backend_instance = {
	.send = fake_send,
	.receive = fake_receive,
};

void occp_fake_backend_init(struct occp_backend **out_backend)
{
	memset(fake_mem, 0, sizeof(fake_mem));
	fake_resp_len = 0;
	*out_backend = &fake_backend_instance;
}

void occp_fake_backend_reset(void)
{
	memset(fake_mem, 0, sizeof(fake_mem));
	fake_resp_len = 0;
}
