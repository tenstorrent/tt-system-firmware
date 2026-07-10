/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/ztest.h>
#include <tenstorrent/occp.h>

void occp_fake_backend_init(struct occp_backend **out_backend);
void occp_fake_backend_reset(void);

static struct occp_backend *backend;

static void *occp_fake_suite_setup(void)
{
	occp_fake_backend_init(&backend);
	return NULL;
}

static void occp_fake_before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	occp_fake_backend_reset();
}

ZTEST(occp_fake, test_write_small)
{
	uint8_t data[4] = {0x01, 0x02, 0x03, 0x04};

	zassert_equal(occp_write_data(backend, 0x0, data, sizeof(data)), 0,
		      "Small write failed");
}

/* 232 = ROUND_DOWN(OCCP_MAX_MSG_SIZE - sizeof(occp_write_data_request), 4) */
ZTEST(occp_fake, test_write_max_single_chunk)
{
	uint8_t data[232];

	memset(data, 0xA5, sizeof(data));
	zassert_equal(occp_write_data(backend, 0x0, data, sizeof(data)), 0,
		      "Max single-chunk write failed");
}

ZTEST(occp_fake, test_write_multi_chunk)
{
	uint8_t data[236];

	memset(data, 0xBB, sizeof(data));
	zassert_equal(occp_write_data(backend, 0x0, data, sizeof(data)), 0,
		      "Multi-chunk write failed");
}

ZTEST(occp_fake, test_read_crc1_boundary)
{
	uint8_t data[4] = {0};

	zassert_equal(occp_read_data(backend, 0x0, data, sizeof(data)), 0,
		      "Read with 1-byte CRC failed");
}

/* Smallest 4-byte-aligned payload above the 13-byte CRC threshold */
ZTEST(occp_fake, test_read_crc4_boundary)
{
	uint8_t data[16] = {0};

	zassert_equal(occp_read_data(backend, 0x0, data, sizeof(data)), 0,
		      "Read with 4-byte CRC failed");
}

/*
 * Key regression test: 252-byte read produces an 8+252+4=264 byte response.
 * With the old 255-byte buffer this would overflow; the resized buffer handles it.
 */
ZTEST(occp_fake, test_read_max_single_chunk)
{
	/* 252 = ROUND_DOWN(OCCP_MAX_MSG_SIZE, 4) */
	uint8_t data[252] = {0};

	zassert_equal(occp_read_data(backend, 0x0, data, sizeof(data)), 0,
		      "Max single-chunk read failed");
}

ZTEST(occp_fake, test_read_multi_chunk)
{
	uint8_t data[256] = {0};

	zassert_equal(occp_read_data(backend, 0x0, data, sizeof(data)), 0,
		      "Multi-chunk read failed");
}

ZTEST(occp_fake, test_write_read_roundtrip_small)
{
	uint8_t write_data[4] = {0xDE, 0xAD, 0xBE, 0xEF};
	uint8_t read_data[4] = {0};

	zassert_equal(occp_write_data(backend, 0x100, write_data, 4), 0, "Write failed");
	zassert_equal(occp_read_data(backend, 0x100, read_data, 4), 0, "Read failed");
	zassert_mem_equal(write_data, read_data, 4, "Roundtrip data mismatch");
}

ZTEST(occp_fake, test_write_read_roundtrip_max)
{
	uint8_t write_data[232];
	uint8_t read_data[232] = {0};

	for (int i = 0; i < 232; i++) {
		write_data[i] = (uint8_t)(i ^ 0x55);
	}
	zassert_equal(occp_write_data(backend, 0x0, write_data, 232), 0, "Write failed");
	zassert_equal(occp_read_data(backend, 0x0, read_data, 232), 0, "Read failed");
	zassert_mem_equal(write_data, read_data, 232, "Max roundtrip mismatch");
}

ZTEST(occp_fake, test_read_write_multi_chunk_verify)
{
	uint8_t write_data[256];
	uint8_t read_data[256] = {0};

	for (int i = 0; i < 256; i++) {
		write_data[i] = (uint8_t)i;
	}
	zassert_equal(occp_write_data(backend, 0x200, write_data, 256), 0, "Write failed");
	zassert_equal(occp_read_data(backend, 0x200, read_data, 256), 0, "Read failed");
	zassert_mem_equal(write_data, read_data, 256, "Multi-chunk roundtrip mismatch");
}

ZTEST_SUITE(occp_fake, NULL, occp_fake_suite_setup, occp_fake_before_each, NULL, NULL);
