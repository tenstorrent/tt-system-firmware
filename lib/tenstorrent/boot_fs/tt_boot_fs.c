/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <tenstorrent/tt_boot_fs.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(tt_boot_fs, CONFIG_TT_APP_LOG_LEVEL);

uint32_t tt_boot_fs_cksum(uint32_t cksum, const uint8_t *data, size_t num_bytes)
{
	if (num_bytes == 0 || data == NULL) {
		return 0;
	}

	/* Always read 1 fewer word, and handle the 4 possible alignment cases outside the loop */
	const uint32_t num_dwords = num_bytes / sizeof(uint32_t) - 1;
	uint32_t *data_as_dwords = (uint32_t *)data;

	for (uint32_t i = 0; i < num_dwords; i++) {
		cksum += *data_as_dwords++;
	}

	switch (num_bytes % 4) {
	case 0:
		cksum += *data_as_dwords & 0xffffffff;
		break;
	default:
		__ASSERT(false, "size %zu is not a multiple of 4", num_bytes);
		break;
	}

	return cksum;
}

/**
 * @brief Validates the embedded checksum of a boot-fs file descriptor.
 *
 * The descriptor stores its checksum in the trailing @p fd_crc word, computed
 * over every preceding byte of the structure. Callers should not have to know
 * about that layout, so this helper handles the sizing and casting itself.
 */
static tt_checksum_res_t check_fd_crc(const tt_boot_fs_fd *fd)
{
	const size_t hashed_len = sizeof(*fd) - sizeof(fd->fd_crc);
	uint32_t calculated = tt_boot_fs_cksum(0, (const uint8_t *)fd, hashed_len);

	return calculated == fd->fd_crc ? TT_BOOT_FS_CHK_OK : TT_BOOT_FS_CHK_FAIL;
}

/**
 * @brief Reads and validates the boot-fs header at @ref TT_BOOT_FS_HEADER_ADDR
 *
 * Images created before the multi-table header was introduced are represented
 * as two tables at the fixed ROM and failover addresses.
 *
 * @param[in]  dev    Flash device holding the boot filesystem.
 * @param[out] header Header populated from flash (or synthesized for legacy images).
 * @param[out] legacy Set to `true` when the image predates the multi-table header.
 *
 * @retval 0      Success, @p header and @p legacy populated.
 * @retval -EIO   Flash read failure.
 * @retval -ENXIO Version or table count mismatch.
 */
static int read_boot_fs_header(const struct device *dev, tt_boot_fs_header *header, bool *legacy)
{
	int ret = flash_read(dev, TT_BOOT_FS_HEADER_ADDR, header, sizeof(*header));

	if (ret < 0) {
		LOG_ERR("%s() failed: %d", "flash_read", ret);
		return -EIO;
	}
	if (header->magic != TT_BOOT_FS_MAGIC) {
		LOG_DBG("No multi-table boot FS header; using legacy table addresses");
		header->version = 0;
		header->table_count = 2;
		*legacy = true;
		return 0;
	}
	if (header->version != TT_BOOT_FS_CURRENT_VERSION) {
		LOG_ERR("Unsupported boot FS version: %d", header->version);
		return -ENXIO;
	}
	if (header->table_count == 0 || header->table_count > TT_BOOT_FS_TABLE_COUNT_MAX) {
		LOG_ERR("Invalid boot FS table count: %u", header->table_count);
		return -ENXIO;
	}

	*legacy = false;
	return 0;
}

/**
 * @brief Reads the @p table_index'th table base address from the boot-fs header
 *
 * @param[in]  dev         Flash device holding the boot filesystem.
 * @param[in]  legacy      `true` if the image predates the multi-table header;
 *                         the two well-known addresses are then synthesized
 *                         instead of read from flash.
 * @param[in]  table_index Index of the table whose address should be read.
 * @param[out] table_addr  Flash address of the requested descriptor table.
 *
 * @retval 0      Success, @p table_addr populated.
 * @retval -EIO   Flash read failure.
 * @retval -ENXIO Table address fails alignment or bounds checks.
 */
static int read_table_addr(const struct device *dev, bool legacy, size_t table_index,
			   uint32_t *table_addr)
{
	if (legacy) {
		*table_addr =
			table_index == 0 ? TT_BOOT_FS_ROM_HEAD_ADDR : TT_BOOT_FS_FAILOVER_HEAD_ADDR;
		return 0;
	}

	uint32_t addr =
		TT_BOOT_FS_HEADER_ADDR + sizeof(tt_boot_fs_header) + table_index * sizeof(uint32_t);
	int ret = flash_read(dev, addr, table_addr, sizeof(*table_addr));

	if (ret < 0) {
		LOG_ERR("%s() failed: %d", "flash_read", ret);
		return -EIO;
	}
	if ((*table_addr % sizeof(uint32_t)) != 0 ||
	    *table_addr > UINT32_MAX - sizeof(tt_boot_fs_fd)) {
		LOG_ERR("Invalid boot FS table address: 0x%08X", *table_addr);
		return -ENXIO;
	}

	return 0;
}

/**
 * @brief Reads and validates a single descriptor at absolute flash address @p fd_addr
 *
 * Streams one descriptor from flash rather than preloading multiple entries, so
 * callers can iterate the file system without committing a large on-stack
 * buffer that would scale with @ref CONFIG_TT_BOOT_FS_IMAGE_COUNT_MAX.
 *
 * @retval 0 If @p fd is populated with a valid descriptor
 * @retval 1 If end of table sentinel; caller should stop iterating this table
 * @retval -EIO Flash read failure
 * @retval -ENXIO Checksum failure
 */
static int read_and_validate_fd(const struct device *dev, uint32_t fd_addr, tt_boot_fs_fd *fd)
{
	int ret = flash_read(dev, fd_addr, fd, sizeof(*fd));

	if (ret < 0) {
		LOG_ERR("%s() failed: %d", "flash_read", ret);
		return -EIO;
	}

	if (fd->flags.f.invalid) {
		return 1;
	}

	if (check_fd_crc(fd) != TT_BOOT_FS_CHK_OK) {
		return -ENXIO;
	}

	return 0;
}

int tt_boot_fs_ls(const struct device *dev, tt_boot_fs_fd *fds, size_t nfds, size_t offset)
{
	if (!dev || !device_is_ready(dev)) {
		return -ENXIO;
	}

	bool count_only = fds == NULL;

	if (!count_only && nfds == 0) {
		return 0;
	}

	tt_boot_fs_header header;
	bool legacy;
	int ret = read_boot_fs_header(dev, &header, &legacy);

	if (ret < 0) {
		return ret;
	}

	/*
	 * input_index counts every valid descriptor encountered across all
	 * tables, so it can be compared against @p offset to skip prefixes.
	 * output_index counts descriptors accepted into @p fds (or, when
	 * @p fds is NULL, the total match count returned to the caller).
	 */
	size_t input_index = 0;
	size_t output_index = 0;

	for (size_t t = 0; t < header.table_count; t++) {
		uint32_t fd_addr;

		ret = read_table_addr(dev, legacy, t, &fd_addr);
		if (ret < 0) {
			return ret;
		}

		/* Stream descriptors from this table until sentinel or configured limit. */
		for (size_t entry = 0; entry < CONFIG_TT_BOOT_FS_IMAGE_COUNT_MAX; entry++) {
			tt_boot_fs_fd fd;

			ret = read_and_validate_fd(dev, fd_addr, &fd);
			if (ret < 0) {
				return ret;
			}
			if (ret == 1) {
				break;
			}

			if (count_only) {
				output_index++;
			} else if (input_index >= offset) {
				fds[output_index++] = fd;
				if (output_index == nfds) {
					return output_index;
				}
			}
			input_index++;
			fd_addr += sizeof(tt_boot_fs_fd);
		}
	}

	return output_index;
}

int tt_boot_fs_find_fd_by_tag(const struct device *flash_dev, const uint8_t *tag, tt_boot_fs_fd *fd)
{
	if (tag == NULL) {
		return -EINVAL;
	}

	if (!flash_dev || !device_is_ready(flash_dev)) {
		return -ENXIO;
	}

	tt_boot_fs_header header;
	bool legacy;
	int ret = read_boot_fs_header(flash_dev, &header, &legacy);

	if (ret < 0) {
		return ret;
	}

	/*
	 * Stream descriptors one at a time across every table advertised by the
	 * boot-fs header. This avoids allocating a CONFIG_TT_BOOT_FS_IMAGE_COUNT_MAX
	 * -sized tt_boot_fs_fd[] on the stack, which is what previously caused a
	 * stack overflow when the descriptor cap was raised.
	 */
	for (size_t t = 0; t < header.table_count; t++) {
		uint32_t fd_addr;

		ret = read_table_addr(flash_dev, legacy, t, &fd_addr);
		if (ret < 0) {
			return ret;
		}

		for (size_t i = 0; i < CONFIG_TT_BOOT_FS_IMAGE_COUNT_MAX; i++) {
			tt_boot_fs_fd cur;

			ret = read_and_validate_fd(flash_dev, fd_addr, &cur);
			if (ret < 0) {
				return ret;
			}
			if (ret == 1) {
				break;
			}

			if (strncmp(tag, cur.image_tag, sizeof(cur.image_tag)) == 0) {
				if (fd != NULL) {
					*fd = cur;
				}
				return 0;
			}

			fd_addr += sizeof(tt_boot_fs_fd);
		}
	}

	return -ENOENT;
}
