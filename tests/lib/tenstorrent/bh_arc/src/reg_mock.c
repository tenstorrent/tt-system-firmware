/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "reg_mock.h"
#include <zephyr/ztest.h>

DEFINE_FFF_GLOBALS;

DEFINE_FAKE_VALUE_FUNC(uint32_t, ReadReg, uint32_t /*addr*/);
DEFINE_FAKE_VOID_FUNC(WriteReg, uint32_t /*addr*/, uint32_t /*val*/);

static void reset_reg(const struct ztest_unit_test *test, void *data)
{
	(void)test;
	(void)data;
	RESET_FAKE(ReadReg);
	RESET_FAKE(WriteReg);
}

ZTEST_RULE(reset_reg_rule, NULL, reset_reg);
