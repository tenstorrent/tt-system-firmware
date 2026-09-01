/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _TT_BOOT_FS_H_
#define _TT_BOOT_FS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/devicetree.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TT_BOOT_FS_HEADER_ADDR             (0x120000)
#define TT_BOOT_FS_ROM_HEAD_ADDR           (0x0)
#define TT_BOOT_FS_SECURITY_BINARY_FD_ADDR (0x3FE0)
#define TT_BOOT_FS_FAILOVER_HEAD_ADDR      (0x4000)
#define TT_BOOT_FS_IMAGE_TAG_SIZE          8
#define TT_BOOT_FS_TABLE_COUNT_MAX         16

struct device;

/**
 * @brief Boot filesystem file flags
 */
typedef struct {
	uint32_t image_size: 24;
	uint32_t invalid: 1;
	uint32_t executable: 1;
	uint32_t fd_flags_rsvd: 6;
} tt_boot_fs_flags;

typedef union {
	uint32_t val;
	tt_boot_fs_flags f;
} tt_boot_fs_flags_u;

typedef struct {
	uint32_t signature_size: 12;
	uint32_t sb_phase: 8; /* 0 - Phase0A, 1 - Phase0B */
} security_fd_flags;

typedef union {
	uint32_t val;
	security_fd_flags f;
} security_fd_flags_u;

/**
 * @brief Boot filesystem file descriptor
 *
 * Describes a binary stored in the boot filesystem.
 */
typedef struct {
	uint32_t spi_addr;
	uint32_t copy_dest;
	tt_boot_fs_flags_u flags;
	uint32_t data_crc;
	security_fd_flags_u security_flags;
	uint8_t image_tag[TT_BOOT_FS_IMAGE_TAG_SIZE];
	uint32_t fd_crc;
} tt_boot_fs_fd;

typedef int (*tt_boot_fs_read)(uint32_t addr, uint32_t size, uint8_t *dst);
typedef int (*tt_boot_fs_write)(uint32_t addr, uint32_t size, const uint8_t *src);
typedef int (*tt_boot_fs_erase)(uint32_t addr, uint32_t size);

typedef struct {
	tt_boot_fs_read hal_spi_read_f;
	tt_boot_fs_write hal_spi_write_f;
	tt_boot_fs_erase hal_spi_erase_f;
} tt_boot_fs;

enum {
	TT_BOOT_FS_OK = 0,
	TT_BOOT_FS_ERR = -1
};

typedef enum {
	TT_BOOT_FS_CHK_OK,
	TT_BOOT_FS_CHK_FAIL,
} tt_checksum_res_t;

/**
 * @brief Multi-table boot filesystem header
 *
 * Located at @ref TT_BOOT_FS_HEADER_ADDR in flash. The three fixed fields are
 * immediately followed on flash by a @p table_count -length array of
 * ``uint32_t`` entries giving the flash address of each descriptor table. The
 * trailing array is not modelled as a flexible array member to keep the type
 * usable from C++ translation units that include this header.
 */
typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t table_count;
} tt_boot_fs_header;

#define TT_BOOT_FS_MAGIC           0x54544246 /* 'TTBF' in ASCII */
#define TT_BOOT_FS_CURRENT_VERSION 1

uint32_t tt_boot_fs_cksum(uint32_t cksum, const uint8_t *data, size_t size);

/**
 * @brief List file descriptors in a boot filesystem
 *
 * Reads up to @p nfds file descriptors from the boot filesystem on flash
 * device @p dev, starting from descriptor index @p offset. Descriptors from
 * every table advertised by the boot filesystem header are streamed in table
 * order, so callers do not need to enumerate tables themselves.
 *
 * If @p fds is `NULL`, this function counts the number of files in the boot
 * filesystem instead of returning any descriptors. In that mode, @p nfds and
 * @p offset are ignored.
 *
 * @param[in]  dev    Flash device containing the boot filesystem
 * @param[out] fds    (optional) Array to store descriptors into. Pass `NULL`
 *                    to count files instead.
 * @param[in]  nfds   Maximum number of descriptors to write into @p fds.
 * @param[in]  offset Descriptor index from which to begin reading.
 *
 * @retval >=0     Number of descriptors read (or total file count when @p fds is `NULL`).
 * @retval -EIO    Flash read failure.
 * @retval -ENXIO  @p dev does not contain a valid boot filesystem.
 */
int tt_boot_fs_ls(const struct device *dev, tt_boot_fs_fd *fds, size_t nfds, size_t offset);

/**
 * @brief Find a boot filesystem file descriptor by name on a given flash device.
 *
 * If @p fd is `NULL`, then a return value of 0 indicates that a file named @p name exists in the
 * boot filesystem residing on @p flash_dev.
 *
 * If @p fd is non-`NULL`, then file descriptor contents are written to the memory pointed to by it.
 * The output file descriptor includes useful information about the file, like
 * - the address of the file in @p flash_dev,
 * - the size of the file, and
 * - the checksum of the file, and more.
 *
 * @param flash_dev flash device containing the boot filesystem
 * @param tag name of the image to search for
 * @param[out] fd optional pointer to memory where the file descriptor will be written, if found
 *
 * @retval 0 on success
 * @retval -EIO if an I/O error occurs
 * @retval -ENXIO if @p flash_dev does not contain a boot filesystem
 * @retval -ENOENT if no file was found matching the specified tag
 */
int tt_boot_fs_find_fd_by_tag(const struct device *flash_dev, const uint8_t *tag,
			      tt_boot_fs_fd *fd);

#ifdef __cplusplus
}
#endif

#endif
