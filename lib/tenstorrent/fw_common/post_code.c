/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <tenstorrent/post_code.h>

#include "reg.h"

#if defined(CONFIG_TT_POST_CODE)
/* Provided by the selected SoC (tt_blackhole today; tt_grendel later). */
#include "status_reg.h"
#endif

void SetPostCode(uint8_t fw_id, uint16_t post_code)
{
#if defined(CONFIG_TT_POST_CODE)
	WriteReg(STATUS_POST_CODE_REG_ADDR,
		 (POST_CODE_PREFIX << 16) | (fw_id << 14) | (post_code & 0x3FFF));
#endif
}
