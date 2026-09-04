/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Keraunos SEP BL1 bring-up. Secure-boot/hash/signature checks are deferred.
 *
 * Consumes a staged BUN2 if one is present, then starts SMC BL0P5 and answers
 * its validation doorbell. Sequence matches app/bl0p5/src/main.c.
 */

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

/* The only staging window defined in this tree. */
#define BUNDLE_WINDOW_ADDR KER_BUN2_STAGING_ADDR
#define BUNDLE_WINDOW_SIZE 0x20000U

static volatile uint32_t *const host_boot_state =
	(volatile uint32_t *)KER_SMC_HOST_BOOT_STATE_ADDR;
static volatile uint32_t *const bundle_validation =
	(volatile uint32_t *)KER_SMC_BUNDLE_VALIDATION_ADDR;

static int start_smc(struct sep_bl1_ctx *ctx, const struct fw_bundle_toc_entry *bl0p5)
{
	struct fw_bundle_toc_entry fallback;

	if (bl0p5 == NULL) {
		/* No bundle to read an entry point from; use the overlay address
		 * BL0P5 is linked at, on the assumption BL0 deposited it there.
		 */
		memset(&fallback, 0, sizeof(fallback));
		fallback.load_addr = KER_SMC_BL0P5_LOAD_ADDR;
		fallback.entry_point = KER_SMC_BL0P5_LOAD_ADDR;
		bl0p5 = &fallback;
	}

	return sep_bl1_start_smc(bl0p5, ctx, KER_SMC_RESET_VECTOR0_ADDR,
				 KER_SMC_RESET_CTRL_ADDR);
}

int main(void)
{
	struct sep_bl1_ctx ctx;
	struct tt_fw_bundle_view bundle;
	const struct fw_bundle_toc_entry *bl0p5 = NULL;
	bool staged;
	int rc;

	sep_keraunos_init_ctx(&ctx);
	sep_bl1_post(&ctx, SEP_BL1_MSG_STATUS, SEP_BL1_STATUS_BOOT_START);
	printk("SEP BL1 bring-up (unsecured) on %s\n", CONFIG_BOARD);

	staged = tt_fw_bundle_parse((const uint8_t *)BUNDLE_WINDOW_ADDR, BUNDLE_WINDOW_SIZE,
				    &bundle) == 0;
	if (!staged) {
		printk("No bundle at 0x%lx; relying on SEP BL0's SMC deposit\n",
		       (unsigned long)BUNDLE_WINDOW_ADDR);
	} else {
		int placed = 0;

		rc = sep_bl1_load_smc_bl0p5(&bundle, &ctx, &bl0p5);
		printk("SMC BL0P5 load rc=%d\n", rc);

		rc = sep_bl1_place_serdes(&bundle, &ctx, KER_SERDES0_SRAM_BASE,
					  KER_SERDES0_SRAM_SIZE, &placed);
		printk("SERDES placement rc=%d images=%d\n", rc, placed);
	}

	rc = start_smc(&ctx, bl0p5);
	printk("SMC start rc=%d\n", rc);
	if (rc != 0) {
		return rc;
	}

	if (staged) {
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
							BUNDLE_WINDOW_SIZE, &ctx, &bundle);
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
