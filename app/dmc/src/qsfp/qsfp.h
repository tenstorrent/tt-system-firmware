/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_DMC_QSFP_H_
#define APP_DMC_QSFP_H_

#include <stdint.h>
#include <tenstorrent/qsfp_mgmt.h>

#ifdef CONFIG_TT_QSFP

/*
 * Probe the four P150A QSFP-DD cages on MCU_I2C0 (the DMC's i2c3): report
 * per-cage module presence and, for any seated module, the CMIS Identifier.
 *
 * Returns a packed status word, one byte per cage (byte 0 = A ... byte 3 = D):
 *   0x00        expander absent (no I2C response)
 *   0x01        expander present, no module seated
 *   0x02        module seated, identifier read at 0x50 failed
 *   0x03        MCU_I2C0 stuck (all four bytes)
 *   0x80 | id   module present; id = SFF-8024 identifier byte
 * Forwarded to the SMC as telemetry TAG_QSFP_STATUS.
 */
uint32_t qsfp_discover(void);

/* Same probe without boot-time discovery logging. Safe to call from the DMC
 * main loop; MCU_I2C0 is shared with the SMC SMBus target.
 */
uint32_t qsfp_poll(void);

/* Execute one packed QSFP_MGMT_REQUEST and fill a bounded response. */
void qsfp_handle_mgmt(uint32_t request, struct qsfp_mgmt_response *response);

/* Deselect every cage while preserving its requested power mode. */
void qsfp_emergency_park(void);

/* Hold FXMA2102 U1 off, isolating the cages from MCU_I2C0. */
void qsfp_hold_translator_off(void);

/* Enable U1 so the cages join MCU_I2C0. */
void qsfp_enable_translator(void);

#else

static inline uint32_t qsfp_discover(void)
{
	return 0;
}

static inline uint32_t qsfp_poll(void)
{
	return 0;
}

static inline void qsfp_handle_mgmt(uint32_t request, struct qsfp_mgmt_response *response)
{
	response->token = QSFP_MGMT_REQUEST_TOKEN(request);
	response->operation = QSFP_MGMT_REQUEST_OP(request);
	response->cage = QSFP_MGMT_REQUEST_CAGE(request);
	response->status = QSFP_MGMT_ERR_UNAVAILABLE;
	response->length = 0;
}

static inline void qsfp_emergency_park(void)
{
}

static inline void qsfp_hold_translator_off(void)
{
}

static inline void qsfp_enable_translator(void)
{
}

#endif /* CONFIG_TT_QSFP */

#endif /* APP_DMC_QSFP_H_ */
