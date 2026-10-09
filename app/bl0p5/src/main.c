/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <tenstorrent/gr_smc.h>

LOG_MODULE_REGISTER(main, CONFIG_TT_APP_LOG_LEVEL);

/* Host <-> SMC boot handshake states, written/polled via the host-boot-state scratch register */
#define HOST_BOOT_STATE_WAIT_FOR_BUNDLE (1U)
#define HOST_BOOT_STATE_BUNDLE_STAGED   (2U)
#define HOST_BOOT_STATE_BUNDLE_CONSUMED (3U)

/* Bundle validation handshake bits, in the bundle-validation scratch register */
#define BUNDLE_READY_FOR_VALIDATION_BIT BIT(0)
#define BUNDLE_VALIDATED_BIT            BIT(1)

/* SMC_CPU_SMC_CPU_CTRL_RESET_VECTOR_0 local address */
#define SMC_RESET_VECTOR0_ADDR 0xC0010000UL

#define HOST_BOOT_STATE_ADDR   ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(host_boot_state)))
#define BUNDLE_VALIDATION_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(bundle_validation)))

static int bl1_launcher_init(void)
{
	/* A) Tell the host we are ready to receive a bundle */
	sys_write32(HOST_BOOT_STATE_WAIT_FOR_BUNDLE, HOST_BOOT_STATE_ADDR);

	/* B) Wait for the host to stage the bundle in the staging area */
	LOG_INF("Waiting for BUN2 staging");
	while ((sys_read32(HOST_BOOT_STATE_ADDR) & 0xF) != HOST_BOOT_STATE_BUNDLE_STAGED) {
		k_busy_wait(100);
	}

	/* Close host access to the staging area; only the SMC touches it from here on */

	/* D) Request validation of the staged bundle */
	sys_write32(BUNDLE_READY_FOR_VALIDATION_BIT, BUNDLE_VALIDATION_ADDR);

	/* E) Wait for validation to complete */
	LOG_INF("Waiting for BUN2 validation");
	while (!(sys_read32(BUNDLE_VALIDATION_ADDR) & BUNDLE_VALIDATED_BIT)) {
		k_busy_wait(100);
	}

	const struct fw_bundle_manifest *manifest =
		(const struct fw_bundle_manifest *)TT_BUN2_STAGING_AREA_ADDR;
	const struct fw_bundle_toc *toc = (const struct fw_bundle_toc *)(TT_BUN2_STAGING_AREA_ADDR +
									 manifest->payload_offset);

	/* Bundles always place the K-SMC BL1 image at TOC index 0 */
	const struct fw_bundle_toc_entry *bl1_entry =
		(toc->image_count > 0) ? &toc->entries[0] : NULL;

	if (bl1_entry == NULL || bl1_entry->type != FW_BUNDLE_IMG_TYPE_SMC_BL1) {
		LOG_ERR("No SMC BL1 image found at TOC index 0 in staged bundle");
		return -ENOENT;
	}

	sys_write32(HOST_BOOT_STATE_BUNDLE_CONSUMED, HOST_BOOT_STATE_ADDR);

	/* F) Copy SMC BL1 from the staged bundle to its execute location */
	memcpy((void *)bl1_entry->load_addr,
	       (const void *)(TT_BUN2_STAGING_AREA_ADDR + manifest->payload_offset +
			      bl1_entry->offset),
	       bl1_entry->length);

	sys_write64(bl1_entry->entry_point, SMC_RESET_VECTOR0_ADDR);

	/* G) Jump to SMC BL1. Does not return; BL1 owns the core from here on. */
	gr_smc_jump_to((uintptr_t)bl1_entry->entry_point);

	return 0;
}

int main(void)
{
	bl1_launcher_init();

	/* Unreachable unless BL1 returns, which means the handoff failed and the core's
	 * state (stack, vector table, PMP, ...) can no longer be trusted.
	 */
	k_panic();
	return 0;
}
