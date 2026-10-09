/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Keraunos SEP BL1 bring-up. Secure-boot/hash/signature checks are deferred.
 *
 * Stages the BUN1 SEP BL0 loaded into the SMC bundle window, loads SMC BL0P5
 * from it and starts the SMC, then answers BL0P5's BUN2 validation doorbell.
 * Sequence matches app/bl0p5/src/main.c.
 */

#include <stdbool.h>
#include <string.h>

#include <zephyr/sys/printk.h>

#include <soc.h>
#include <tenstorrent/sep_bl1.h>

#ifndef CONFIG_SOC_TT_KERAUNOS_SEP

int main(void)
{
	printk("SEP BL1 host-only build; use tt_mmk/tt_keraunos/sep\n");
	return 0;
}

#else

#define BUNDLE_WINDOW_ADDR KER_BUNDLE_STAGING_ADDR
#define BUNDLE_WINDOW_SIZE KER_BUNDLE_STAGING_SIZE

static volatile uint32_t *const host_boot_state = (volatile uint32_t *)KER_SMC_HOST_BOOT_STATE_ADDR;
static volatile uint32_t *const bundle_validation =
	(volatile uint32_t *)KER_SMC_BUNDLE_VALIDATION_ADDR;

static int stage_bun1(const struct sep_bl1_ctx *ctx)
{
	const struct fw_bundle_manifest *m;
	const uint8_t *payload;
	uint32_t addr;
	int rc;

	rc = sep_bl1_bl0_manifest_addr((const uint8_t *)SEP_BL0_STATE_END, &addr);
	if (rc != 0) {
		return rc;
	}

	/* BL0 left payload_offset relative to its SEP SRAM copy; it may be negative. */
	m = (const struct fw_bundle_manifest *)(uintptr_t)addr;
	payload = (const uint8_t *)(uintptr_t)(addr + (uint32_t)m->payload_offset);
	return sep_bl1_stage_bundle(m, payload, (uint8_t *)BUNDLE_WINDOW_ADDR, BUNDLE_WINDOW_SIZE,
				    ctx);
}

static int start_smc(struct sep_bl1_ctx *ctx, const struct fw_bundle_toc_entry *bl0p5)
{
	struct fw_bundle_toc_entry fallback;

	if (bl0p5 == NULL) {
		/* No bundle to read an entry point from; use the overlay address
		 * BL0P5 is linked at, on the assumption something deposited it there.
		 */
		memset(&fallback, 0, sizeof(fallback));
		fallback.load_addr = KER_SMC_BL0P5_ENTRY;
		fallback.entry_point = KER_SMC_BL0P5_ENTRY;
		bl0p5 = &fallback;
	}

	return sep_bl1_start_smc(bl0p5, ctx, KER_SMC_RESET_VECTOR0_ADDR, KER_SMC_RESET_CTRL_ADDR);
}

int main(void)
{
	struct sep_bl1_ctx ctx;
	const struct fw_bundle_toc *toc;
	const struct fw_bundle_toc_entry *bl0p5 = NULL;
	bool from_bl0;
	bool staged;
	int rc;

	sep_keraunos_init_ctx(&ctx);
	sep_bl1_post(&ctx, SEP_BL1_MSG_STATUS, SEP_BL1_STATUS_BOOT_START);
	printk("SEP BL1 bring-up (unsecured) on %s\n", CONFIG_BOARD);

	rc = stage_bun1(&ctx);
	from_bl0 = rc == 0;
	printk("BUN1 staging from SEP BL0 rc=%d\n", rc);

	toc = sep_bl1_bundle_toc((const uint8_t *)BUNDLE_WINDOW_ADDR, BUNDLE_WINDOW_SIZE);
	staged = toc != NULL;
	if (!staged) {
		printk("No bundle at 0x%lx; relying on SMC BL0P5 already being in place\n",
		       (unsigned long)BUNDLE_WINDOW_ADDR);
	} else {
		printk("Bundle at 0x%lx: %u images\n", (unsigned long)BUNDLE_WINDOW_ADDR,
		       (unsigned int)toc->image_count);
		if (from_bl0 && toc->image_count <= SEP_BL1_BUN1_SERDES_TOC_INDEX) {
			printk("BUN1 has no SERDES image at TOC[%u]; BL0P5 PCIe init will fail\n",
			       SEP_BL1_BUN1_SERDES_TOC_INDEX);
		}

		rc = sep_bl1_load_smc_bl0p5((const uint8_t *)BUNDLE_WINDOW_ADDR, BUNDLE_WINDOW_SIZE,
					    &ctx, &bl0p5);
		printk("SMC BL0P5 load rc=%d\n", rc);
		if (rc != 0) {
			sep_console_verdict(false);
			return rc;
		}
	}

	rc = start_smc(&ctx, bl0p5);
	printk("SMC start rc=%d\n", rc);
	if (rc != 0) {
		sep_console_verdict(false);
		return rc;
	}

	/* A BUN1 from BL0 is not BUN2: the host stages BUN2 over it and says so. */
	if (staged && !from_bl0) {
		*host_boot_state = SEP_BL1_HOST_BOOT_STATE_BUNDLE_STAGED;
	}

	printk("Waiting for BUN2 validation doorbell at 0x%lx\n",
	       (unsigned long)KER_SMC_BUNDLE_VALIDATION_ADDR);
	for (;;) {
		uint32_t scratch = *bundle_validation;

		if ((scratch & SEP_BL1_BUNDLE_READY_FOR_VALIDATION_BIT) != 0U &&
		    (scratch & SEP_BL1_BUNDLE_VALIDATED_BIT) == 0U) {
			rc = sep_bl1_ack_bun2_unsecured(&scratch,
							(const uint8_t *)BUNDLE_WINDOW_ADDR,
							BUNDLE_WINDOW_SIZE, &ctx);
			*bundle_validation = scratch;
			printk("BUN2 ACK rc=%d scratch=0x%x\n", rc, scratch);
			break;
		}
		for (volatile int spin = 0; spin < 1000; spin++) {
		}
	}

	sep_bl1_post(&ctx, SEP_BL1_MSG_STATUS, SEP_BL1_STATUS_BOOT_COMPLETE);
	printk("SEP BL1 handoff complete; host_boot_state=0x%x\n", *host_boot_state);
	sep_console_verdict(rc == 0);
	return 0;
}

#endif
