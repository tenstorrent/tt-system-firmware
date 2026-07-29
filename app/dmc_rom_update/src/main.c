/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#define MCUBOOT_PART_NODE  DT_NODE_BY_FIXED_PARTITION_LABEL(mcuboot)
#define BLUPDATE_PART_NODE DT_NODE_BY_FIXED_PARTITION_LABEL(blupdate)
#define DMFW_PART_NODE     DT_NODE_BY_FIXED_PARTITION_LABEL(dmfw)

BUILD_ASSERT(DT_FIXED_PARTITION_EXISTS(MCUBOOT_PART_NODE),
	     "No mcuboot partition found in devicetree");
BUILD_ASSERT(DT_FIXED_PARTITION_EXISTS(BLUPDATE_PART_NODE),
	     "No blupdate partition found in devicetree");
BUILD_ASSERT(DT_FIXED_PARTITION_EXISTS(DMFW_PART_NODE), "No dmfw partition found in devicetree");

uint8_t flash_copy_buf[4 * 1024] __aligned(4);

int main(void)
{
	const struct device *tgt_flash = PARTITION_NODE_DEVICE(MCUBOOT_PART_NODE);
	const struct device *src_flash = PARTITION_NODE_DEVICE(BLUPDATE_PART_NODE);
	int rc;
	off_t tgt_off, src_off, dmfw_off;
	size_t len, erase_len;

	/*
	 * Simply copy the flash data from the bl_update partition to
	 * the MCUBoot partition. There isn't really a recovery path here
	 * if something goes wrong...
	 */
	printk("Starting DMFW rom update...\n");
	tgt_off = DT_REG_ADDR(MCUBOOT_PART_NODE);
	src_off = DT_REG_ADDR(BLUPDATE_PART_NODE);
	dmfw_off = DT_REG_ADDR(DMFW_PART_NODE);
	len = PARTITION_NODE_SIZE(BLUPDATE_PART_NODE);
	/* Erase an additional 0x200 bytes so the new slot0 header will be cleared */
	erase_len = len + 0x200;

	/*
	 * Erase first sector of new DMFW slot1. This ensures that mcuboot won't see
	 * a stale image magic in the slot during the bootstrap.
	 */
	printk("Erasing flash at 0x%lx, size 0x%zx\n", dmfw_off, 0x1000);
	rc = flash_erase(src_flash, dmfw_off, 0x1000);
	if (rc != 0) {
		printk("Flash erase failed: %d\n", rc);
		return rc;
	}
	printk("Erasing flash at 0x%lx, size 0x%zx\n", tgt_off, erase_len);
	rc = flash_erase(tgt_flash, tgt_off, erase_len);
	if (rc != 0) {
		printk("Flash erase failed: %d\n", rc);
		return rc;
	}
	printk("Copying 0x%zx bytes from 0x%lx to 0x%lx\n", len, src_off, tgt_off);
	rc = flash_copy(src_flash, src_off, tgt_flash, tgt_off, len, flash_copy_buf,
			sizeof(flash_copy_buf));
	if (rc != 0) {
		printk("Flash copy failed: %d\n", rc);
		return rc;
	}
	printk("DMFW rom update complete\n");
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}
