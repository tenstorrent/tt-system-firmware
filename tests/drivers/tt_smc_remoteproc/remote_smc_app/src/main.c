/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/autoconf.h>
#include <soc.h>

int main(void)
{
	printf("Secondary BL1 is running on " CONFIG_BOARD_TARGET "!\n");

	test_pass();
	return 0;
}
