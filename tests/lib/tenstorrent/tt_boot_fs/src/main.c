/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/devicetree.h>
#include <string.h>
#include <tenstorrent/tt_boot_fs.h>

#define FLASH_NODE DT_NODELABEL(flashcontroller0)

#define TT_BOOT_FS_FD_HEAD_ADDR 0x0

const struct device *FLASH_DEVICE = DEVICE_DT_GET(FLASH_NODE);

#define MAX_FDS CONFIG_TT_BOOT_FS_IMAGE_COUNT_MAX

#define IMAGE_ADDR     0x14000
#define TEST_ALIGNMENT 0x1000

static void publish_bootfs_header(uint32_t version, uint32_t table_count)
{
	int rc = flash_erase(FLASH_DEVICE, TT_BOOT_FS_HEADER_ADDR, 4096);

	zassert_equal(rc, 0, "Failed to erase bootfs header area in flash");

	tt_boot_fs_header header = {
		.magic = TT_BOOT_FS_MAGIC,
		.version = version,
		.table_count = table_count,
	};

	rc = flash_write(FLASH_DEVICE, TT_BOOT_FS_HEADER_ADDR, &header, sizeof(header));
	zassert_equal(rc, 0, "Failed to write bootfs header to flash");

	uint32_t table_addr = TT_BOOT_FS_FD_HEAD_ADDR;

	rc = flash_write(FLASH_DEVICE, TT_BOOT_FS_HEADER_ADDR + sizeof(header), &table_addr,
			 sizeof(table_addr));
	zassert_equal(rc, 0, "Failed to write bootfs table address to flash");
}

static void setup_fd(tt_boot_fs_fd *fd, uint32_t spi_addr, uint32_t copy_dest, uint32_t flags,
		     const char *tag, const uint8_t *img, size_t img_len)
{
	memset(fd, 0, sizeof(*fd));
	fd->spi_addr = spi_addr;
	fd->copy_dest = copy_dest;
	fd->flags.val = flags;
	fd->data_crc = tt_boot_fs_cksum(0, img, img_len);
	fd->security_flags.val = 0;
	memset(fd->image_tag, 0, TT_BOOT_FS_IMAGE_TAG_SIZE);
	memcpy(fd->image_tag, tag, strlen(tag));
	fd->fd_crc = 0;
}

static void *setup_bootfs(void)
{
	printk("FLASH_DEVICE: %p\n", FLASH_DEVICE);
	printk("Flash device name: %s\n", FLASH_DEVICE->name);
	printk("Flash device ready: %s\n", device_is_ready(FLASH_DEVICE) ? "YES" : "NO");
	zassert_not_null(FLASH_DEVICE, "FLASH_DEVICE is NULL!");

	static tt_boot_fs_fd fds[3];
	uint8_t image_A[] = {0x73, 0x73, 0x42, 0x42};
	uint8_t image_B[] = {0x73, 0x73, 0x42, 0x42, 0x37, 0x37, 0x24, 0x24};
	uint8_t image_C[] = {0x73, 0x73, 0x42, 0x42};

	uint32_t spi_addr = IMAGE_ADDR;

#define ALIGN_UP(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

	setup_fd(&fds[0], spi_addr, 0x1000000, (sizeof(image_A) & 0xFFFFFF) | (1 << 25), "imageA",
		 image_A, sizeof(image_A));
	spi_addr += ALIGN_UP(sizeof(image_A), TEST_ALIGNMENT);

	setup_fd(&fds[1], spi_addr, 0, (sizeof(image_B) & 0xFFFFFF), "imageB", image_B,
		 sizeof(image_B));
	spi_addr += ALIGN_UP(sizeof(image_B), TEST_ALIGNMENT);

	setup_fd(&fds[2], spi_addr, 0x1000000, (sizeof(image_C) & 0xFFFFFF), "failover", image_C,
		 sizeof(image_C));

	for (size_t i = 0; i < 3; ++i) {
		fds[i].fd_crc = tt_boot_fs_cksum(0, (uint8_t *)&fds[i],
						 sizeof(tt_boot_fs_fd) - sizeof(fds[i].fd_crc));
	}

	uint32_t erase_size =
		(spi_addr + ALIGN_UP(sizeof(image_C), TEST_ALIGNMENT)) - TT_BOOT_FS_FD_HEAD_ADDR;
	int rc = flash_erase(FLASH_DEVICE, TT_BOOT_FS_FD_HEAD_ADDR, ROUND_UP(erase_size, 4096));

	zassert_equal(rc, 0, "Failed to erase test bootfs area in flash");

	rc = flash_write(FLASH_DEVICE, TT_BOOT_FS_FD_HEAD_ADDR, &fds[0], sizeof(tt_boot_fs_fd));
	zassert_equal(rc, 0, "Failed to write fd[0] to flash");
	rc = flash_write(FLASH_DEVICE, TT_BOOT_FS_FD_HEAD_ADDR + sizeof(tt_boot_fs_fd), &fds[1],
			 sizeof(tt_boot_fs_fd));
	zassert_equal(rc, 0, "Failed to write fd[1] to flash");
	rc = flash_write(FLASH_DEVICE, TT_BOOT_FS_FD_HEAD_ADDR + 2 * sizeof(tt_boot_fs_fd), &fds[2],
			 sizeof(tt_boot_fs_fd));
	zassert_equal(rc, 0, "Failed to write fd[2] to flash");

	tt_boot_fs_fd invalid_fd = {0};

	invalid_fd.flags.f.invalid = 1;
	invalid_fd.fd_crc = tt_boot_fs_cksum(0, (uint8_t *)&invalid_fd,
					     sizeof(tt_boot_fs_fd) - sizeof(invalid_fd.fd_crc));
	rc = flash_write(FLASH_DEVICE, TT_BOOT_FS_FD_HEAD_ADDR + 3 * sizeof(tt_boot_fs_fd),
			 &invalid_fd, sizeof(tt_boot_fs_fd));
	zassert_equal(rc, 0, "Failed to write invalid FD to flash");

	rc = flash_write(FLASH_DEVICE, fds[0].spi_addr, image_A, sizeof(image_A));
	zassert_equal(rc, 0, "Failed to write image_A to flash");
	rc = flash_write(FLASH_DEVICE, fds[1].spi_addr, image_B, sizeof(image_B));
	zassert_equal(rc, 0, "Failed to write image_B to flash");
	rc = flash_write(FLASH_DEVICE, fds[2].spi_addr, image_C, sizeof(image_C));
	zassert_equal(rc, 0, "Failed to write image_C to flash");

	publish_bootfs_header(TT_BOOT_FS_CURRENT_VERSION, 1);

	return NULL;
}

/* all input must be aligned to a 4-byte boundary and be a multiple of 4 bytes */
__aligned(sizeof(uint32_t)) static const uint8_t one_byte[] = {0x42};
/*
 * __aligned(sizeof(uint32_t)) static const uint8_t two_bytes[] = {
 *   0x42,
 *   0x42,
 * };
 * __aligned(sizeof(uint32_t)) static const uint8_t three_bytes[] = {
 *   0x73,
 *   0x42,
 *   0x42,
 * };
 */
static const uint32_t four_bytes = 0x42427373;
/*
 * __aligned(sizeof(uint32_t)) static const uint8_t five_bytes[] = {
 *   0x73, 0x73, 0x42, 0x42, 0x37,
 * };
 * __aligned(sizeof(uint32_t)) static const uint8_t six_bytes[] = {
 *   0x73, 0x73, 0x42, 0x42, 0x37, 0x37,
 * };
 * __aligned(sizeof(uint32_t)) static const uint8_t seven_bytes[] = {
 *   0x73, 0x73, 0x42, 0x42, 0x37, 0x37, 0x24,
 * };
 */
static const uint64_t eight_bytes = 0x2424373742427373;

ZTEST(tt_boot_fs, test_tt_boot_fs_cksum)
{
	uint32_t cksum;

	static const struct harness_data {
		uint32_t expect;
		const uint8_t *data;
		size_t size;
	} harness[] = {
		{0, NULL, 0},
		{0, one_byte, 0},
		/*
		 * {0x00000042, one_byte, 1},
		 * {0x00004242, two_bytes, 2},
		 * {0x00000073, three_bytes, 3},
		 */
		{0x42427373, (uint8_t *)&four_bytes, 4},
		/*
		 *{0x4284e6e6, five_bytes, 5},
		 *{0x4242e6e6, six_bytes, 6},
		 *{0x424273e6, seven_bytes, 7},
		 */
		{0x6666aaaa, (uint8_t *)&eight_bytes, 8},
	};

	ARRAY_FOR_EACH_PTR(harness, it) {
		cksum = tt_boot_fs_cksum(0, it->data, it->size);
		zassert_equal(it->expect, cksum, "%d: expected: %08x actual: %08x", it - harness,
			      it->expect, cksum);
	}
}

#define DECL_TEST_SPEC(d, f, n, o, e)                                                              \
	(struct test_spec)                                                                         \
	{                                                                                          \
		.dev = d, .fds = f, .nfds = n, .offset = o, .expect = e                            \
	}

ZTEST(tt_boot_fs, test_boot_fs_ls)
{
	static tt_boot_fs_fd fds[MAX_FDS];
	const struct device *valid_dev = FLASH_DEVICE;
	const struct device *null_dev = NULL;

	const int total_valid_fds_on_flash = 3;

	struct test_spec {
		const struct device *dev;
		tt_boot_fs_fd *fds;
		size_t nfds;
		size_t offset;
		int expect;
	} specs[10];

	size_t i = 0;

	specs[i++] = DECL_TEST_SPEC(null_dev, fds, MAX_FDS, 0, -ENXIO);
	specs[i++] = DECL_TEST_SPEC(valid_dev, NULL, MAX_FDS, 0, total_valid_fds_on_flash);
	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, 0, 0, 0);
	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, 1, 0, 1);

	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, MAX_FDS, 0, total_valid_fds_on_flash);
	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, 2, 0, 2);

	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, MAX_FDS, 1, 2);
	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, MAX_FDS, 2, 1);
	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, MAX_FDS, 3, 0);
	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, MAX_FDS, 4, 0);

	ARRAY_FOR_EACH(specs, i) {
		struct test_spec *spec = &specs[i];

		int actual = tt_boot_fs_ls(spec->dev, spec->fds, spec->nfds, spec->offset);

		zassert_equal(
			actual, spec->expect,
			"Case %zu: tt_boot_fs_ls(dev:%p, fds:%p, nfds:%zu, offset:%zu) failed. "
			"Got %d, expected %d",
			i, spec->dev, spec->fds, spec->nfds, spec->offset, actual, spec->expect);
	}
}

#define DECL_TEST_FIND_SPEC(d, t, f, e)                                                            \
	(struct test_spec)                                                                         \
	{                                                                                          \
		.dev = (d), .tag = (t), .fd_out = (f), .expect = (e)                               \
	}

ZTEST(tt_boot_fs, test_find_fd_by_tag)
{
	tt_boot_fs_fd result_fd;
	const struct device *valid_dev = FLASH_DEVICE;
	const struct device *null_dev = NULL;

	const uint8_t found_tag[8] = "imageA";
	const uint8_t not_found_tag[8] = "notFound";

	struct test_spec {
		const struct device *dev;
		const uint8_t *tag;
		tt_boot_fs_fd *fd_out;
		int expect;
	} specs[6];

	size_t i = 0;

	specs[i++] = DECL_TEST_FIND_SPEC(null_dev, found_tag, &result_fd, -ENXIO);
	specs[i++] = DECL_TEST_FIND_SPEC(valid_dev, found_tag, NULL, 0);
	specs[i++] = DECL_TEST_FIND_SPEC(valid_dev, not_found_tag, NULL, -ENOENT);
	specs[i++] = DECL_TEST_FIND_SPEC(valid_dev, NULL, &result_fd, -EINVAL);
	specs[i++] = DECL_TEST_FIND_SPEC(valid_dev, found_tag, &result_fd, 0);
	specs[i++] = DECL_TEST_FIND_SPEC(valid_dev, not_found_tag, &result_fd, -ENOENT);

	ARRAY_FOR_EACH(specs, i) {
		struct test_spec *spec = &specs[i];

		int actual = tt_boot_fs_find_fd_by_tag(spec->dev, spec->tag, spec->fd_out);

		zassert_equal(actual, spec->expect,
			      "Case %zu: find(tag:\"%s\") failed. Got %d, expected %d", i,
			      spec->tag, actual, spec->expect);
		if (actual == 0 && spec->fd_out != NULL) {
			zassert_mem_equal(spec->fd_out->image_tag, found_tag,
					  TT_BOOT_FS_IMAGE_TAG_SIZE,
					  "Case %zu: Returned FD tag does not match", i);
		}
	}
}

ZTEST(tt_boot_fs, test_header_validation_and_legacy_fallback)
{
	const uint8_t found_tag[8] = "imageA";

	/* An erased header represents an image from before the multi-table format. */
	int rc = flash_erase(FLASH_DEVICE, TT_BOOT_FS_HEADER_ADDR, 4096);

	zassert_equal(rc, 0, "Failed to erase bootfs header area in flash");
	zassert_equal(tt_boot_fs_find_fd_by_tag(FLASH_DEVICE, found_tag, NULL), 0,
		      "Legacy fixed table addresses were not used");

	publish_bootfs_header(TT_BOOT_FS_CURRENT_VERSION, TT_BOOT_FS_TABLE_COUNT_MAX + 1);
	zassert_equal(tt_boot_fs_find_fd_by_tag(FLASH_DEVICE, found_tag, NULL), -ENXIO,
		      "An excessive table count should be rejected");

	/* Restore the valid header for any test that runs after this one. */
	publish_bootfs_header(TT_BOOT_FS_CURRENT_VERSION, 1);
}

ZTEST_SUITE(tt_boot_fs, NULL, setup_bootfs, NULL, NULL, NULL);
