/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/drivers/gpio.h>
#include <stdlib.h>

#include <tenstorrent/jtag_bootrom.h>
#include <zephyr/drivers/jtag.h>

static struct bh_chip test_chip = {.config = {
					   .jtag = DEVICE_DT_GET(DT_PATH(jtag)),
					   .asic_reset = GPIO_DT_SPEC_GET(DT_PATH(mcureset), gpios),
					   .spi_reset = GPIO_DT_SPEC_GET(DT_PATH(spireset), gpios),
					   .pgood = GPIO_DT_SPEC_GET(DT_PATH(pgood), gpios),
				   }};

ZTEST(jtag_bootrom, test_jtag_bootrom)
{
	const uint32_t *const patch = (const uint32_t *)get_bootcode();
	const size_t patch_len = get_bootcode_len();

	zassert_ok(jtag_bootrom_patch_offset(&test_chip, patch, patch_len, 0));
	zassert_ok(jtag_bootrom_verify(test_chip.config.jtag, patch, patch_len));
}

static void before(void *arg)
{
	ARG_UNUSED(arg);

	/* discarded if no zephyr,gpio-emul exists or if CONFIG_JTAG_VERIFY_WRITE=n */
	__aligned(sizeof(uint32_t)) uint8_t *sram = malloc(get_bootcode_len() * sizeof(uint32_t));
	const size_t patch_len = get_bootcode_len();

	zassert_ok(jtag_bootrom_init(&test_chip));
	zassert_ok(jtag_bootrom_reset_asic(&test_chip));

	if (IS_ENABLED(CONFIG_JTAG_EMUL)) {
		jtag_emul_setup(test_chip.config.jtag, (uint32_t *)sram, patch_len);
	}
}

static void after(void *arg)
{
	ARG_UNUSED(arg);

	jtag_bootrom_teardown(&test_chip);
}

ZTEST_SUITE(jtag_bootrom, NULL, NULL, before, after, NULL);
