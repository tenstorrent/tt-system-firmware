/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TENSTORRENT_GR_SMC_AVS_GR_H_
#define TENSTORRENT_GR_SMC_AVS_GR_H_

#include <stdint.h>

/* AVSBus rail of the SMC core supply on the board VR controller. */
#define GR_AVS_RAIL_VCORE 0

/* AVSBus read commands and the units the VR controller reports them in. */
enum gr_avs_read_cmd {
	GR_AVS_READ_VOLTAGE = 0x0,     /* mV */
	GR_AVS_READ_CURRENT = 0x2,     /* 10 mA */
	GR_AVS_READ_TEMPERATURE = 0x3, /* 0.1 degC, signed */
};

/*
 * Release the SMC apb2avsbus controller from reset and bring up the AVS clock.
 * Returns 0, -ENODEV if the controller does not respond, or -ETIMEDOUT if the
 * slave resync does not complete.
 */
int gr_avs_init(void);

/*
 * Issue one AVSBus read and return its 16-bit payload. Returns 0, -ETIMEDOUT
 * if the bus does not answer, or -EIO if the slave rejects the command.
 */
int gr_avs_read(uint8_t rail, enum gr_avs_read_cmd cmd, uint16_t *data);

#endif /* TENSTORRENT_GR_SMC_AVS_GR_H_ */
