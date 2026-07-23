/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <tenstorrent/post_code.h>
#include "reg.h"
#include "status_reg.h"

void SetPostCode(uint8_t fw_id, uint16_t post_code)
{
#if defined(CONFIG_TT_POST_CODE)
	WriteReg(RESET_UNIT_SCRATCH_REG_ADDR(0),
		 (POST_CODE_PREFIX << 16) | (fw_id << 14) | (post_code & 0x3FFF));
#endif
}
