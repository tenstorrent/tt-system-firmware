/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef INCLUDE_TENSTORRENT_QSFP_MGMT_H_
#define INCLUDE_TENSTORRENT_QSFP_MGMT_H_

#include <stdint.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QSFP_CAGE_COUNT          4U
#define QSFP_MGMT_PAYLOAD_SIZE   20U
#define QSFP_MGMT_SMC_TIMEOUT_MS 5000U

/*
 * TAG_QSFP_STATUS contains one byte per cage (A in bits 7:0). Values with the
 * present flag set carry the module's seven-bit SFF-8024 identifier.
 */
enum qsfp_telemetry_status {
	QSFP_TELEM_EXPANDER_ABSENT = 0x00,
	QSFP_TELEM_NO_MODULE = 0x01,
	QSFP_TELEM_CMIS_READ_FAILED = 0x02,
	QSFP_TELEM_BUS_STUCK = 0x03,
	QSFP_TELEM_PRESENT_FLAG = 0x80,
};

#define QSFP_TELEM_CAGE(word, cage) ((uint8_t)((uint32_t)(word) >> (8U * (cage))))
#define QSFP_TELEM_PRESENT_ID(identifier)                                                          \
	((uint8_t)(QSFP_TELEM_PRESENT_FLAG | ((identifier) & 0x7fU)))

enum qsfp_mgmt_op {
	QSFP_MGMT_OP_STATUS = 0,
	QSFP_MGMT_OP_INVENTORY = 1,
	/* Moves the LPMODE pin. Empty cages are allowed; poll STATUS for CMIS Ready. */
	QSFP_MGMT_OP_SET_POWER = 2,
	QSFP_MGMT_OP_RESET = 3,
	QSFP_MGMT_OP_DOM_MODULE = 4,
	QSFP_MGMT_OP_DOM_LANE = 5,
	/* Raw upper-memory read; see QSFP_MGMT_PAGE_ARG. */
	QSFP_MGMT_OP_READ_PAGE = 6,
	QSFP_MGMT_OP_COUNT,
};

/*
 * Reads QSFP_MGMT_PAYLOAD_SIZE bytes. Upper pages are CMIS 00h/01h/02h/10h/11h;
 * QSFP_MGMT_PAGE_LOWER is bytes 0-127. Missing pages return UNAVAILABLE.
 */
#define QSFP_MGMT_PAGE_ARG(page_index, block) ((uint8_t)((page_index) << 4 | (block)))
#define QSFP_MGMT_PAGE_ARG_INDEX(arg)         ((uint8_t)((arg) >> 4))
#define QSFP_MGMT_PAGE_ARG_BLOCK(arg)         ((uint8_t)((arg) & 0x0f))

enum qsfp_mgmt_page_index {
	QSFP_MGMT_PAGE_00 = 0,
	QSFP_MGMT_PAGE_01 = 1,
	QSFP_MGMT_PAGE_02 = 2,
	QSFP_MGMT_PAGE_10 = 3,
	QSFP_MGMT_PAGE_11 = 4,
	/* Lower memory bytes 0-127, which exists on every module. */
	QSFP_MGMT_PAGE_LOWER = 5,
	QSFP_MGMT_PAGE_COUNT,
};

enum qsfp_inventory_field {
	QSFP_INV_IDENTIFIER = 0,
	QSFP_INV_VENDOR_NAME,
	QSFP_INV_VENDOR_OUI,
	QSFP_INV_VENDOR_PN,
	QSFP_INV_VENDOR_REV,
	QSFP_INV_VENDOR_SN,
	QSFP_INV_DATE_CODE,
	QSFP_INV_CONNECTOR,
	QSFP_INV_MEDIA_TYPE,
	/*
	 * The eight CMIS application descriptors at lower page bytes 86-117 are
	 * four bytes each, which does not fit one QSFP_MGMT_PAYLOAD_SIZE reply,
	 * so they are split: APPLICATIONS carries the host/media ID pairs and
	 * APP_LANES the lane-count and host-lane-assignment pairs.
	 */
	QSFP_INV_APPLICATIONS,
	QSFP_INV_APP_LANES,
	QSFP_INV_COUNT,
};

enum qsfp_power_mode {
	QSFP_POWER_LOW = 0,
	QSFP_POWER_HIGH = 1,
};

enum qsfp_mgmt_status {
	QSFP_MGMT_OK = 0,
	QSFP_MGMT_ERR_ARGUMENT = 1,
	QSFP_MGMT_ERR_ABSENT = 2,
	QSFP_MGMT_ERR_I2C = 3,
	QSFP_MGMT_ERR_TIMEOUT = 4,
	QSFP_MGMT_ERR_BUSY = 5,
	QSFP_MGMT_ERR_UNAVAILABLE = 6,
	QSFP_MGMT_ERR_POWER = 7,
	QSFP_MGMT_ERR_PROTOCOL = 8,
};

/*
 * The host request is struct qsfp_mgmt_rqst in msgqueue.h. The SMC validates
 * operation/cage, allocates a non-zero token, and converts that request to the
 * single uint32_t payload available on CM2DM. The DMC returns the same token,
 * operation, and cage in qsfp_mgmt_response so stale replies can be ignored.
 */
#define QSFP_MGMT_REQUEST(op, cage, arg, token)                                                    \
	((uint32_t)(op) | ((uint32_t)(cage) << 8) | ((uint32_t)(arg) << 16) |                      \
	 ((uint32_t)(token) << 24))
#define QSFP_MGMT_REQUEST_OP(data)    ((uint8_t)(data))
#define QSFP_MGMT_REQUEST_CAGE(data)  ((uint8_t)((data) >> 8))
#define QSFP_MGMT_REQUEST_ARG(data)   ((uint8_t)((data) >> 16))
#define QSFP_MGMT_REQUEST_TOKEN(data) ((uint8_t)((data) >> 24))

/*
 * Exactly 28 bytes, so it fits in response.data[1..7]. It is also the payload
 * of CMFW_SMBUS_QSFP_MGMT_RESPONSE.
 */
struct qsfp_mgmt_response {
	uint8_t token;
	uint8_t operation;
	uint8_t cage;
	uint8_t status;
	uint8_t length;
	uint8_t reserved[3];
	uint8_t payload[QSFP_MGMT_PAYLOAD_SIZE];
} __packed;

/** Payload returned by QSFP_MGMT_OP_STATUS. */
struct qsfp_status_payload {
	uint8_t pins;
	uint8_t identifier;
	uint8_t revision;
	uint8_t power_mode;
	uint8_t module_state;
} __packed;

struct qsfp_dom_module {
	int16_t temperature_256c;
	uint16_t vcc_100uv;
	uint8_t module_state;
	uint8_t low_power;
	uint8_t interrupt_asserted;
	uint8_t reserved;
} __packed;

struct qsfp_dom_lane {
	uint16_t tx_bias_2ua;
	uint16_t tx_power_01uw;
	uint16_t rx_power_01uw;
	uint8_t tx_bias_multiplier;
	uint8_t monitor_flags;
} __packed;

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_TENSTORRENT_QSFP_MGMT_H_ */
