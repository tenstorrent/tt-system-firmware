/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-requested QSFP-DD management operations.
 */

#include "qsfp.h"
#include "qsfp_bus.h"
#include "qsfp_cmis.h"

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

static uint8_t dom_bias_multiplier[QSFP_CAGE_COUNT];
static uint8_t dom_monitor_flags[QSFP_CAGE_COUNT];

void qsfp_dom_cache_invalidate(uint8_t cage)
{
	dom_bias_multiplier[cage] = 0;
	dom_monitor_flags[cage] = 0;
}

static void qsfp_response_init(uint32_t request, struct qsfp_mgmt_response *response)
{
	memset(response, 0, sizeof(*response));
	response->token = QSFP_MGMT_REQUEST_TOKEN(request);
	response->operation = QSFP_MGMT_REQUEST_OP(request);
	response->cage = QSFP_MGMT_REQUEST_CAGE(request);
}

static int qsfp_page_status(int ret)
{
	if (ret == 0) {
		return QSFP_MGMT_OK;
	}
	if (ret == -ENOTSUP) {
		return QSFP_MGMT_ERR_UNAVAILABLE;
	}
	return QSFP_MGMT_ERR_I2C;
}

static void qsfp_response_copy(struct qsfp_mgmt_response *response, const void *data, size_t len)
{
	response->length = MIN(len, sizeof(response->payload));
	memcpy(response->payload, data, response->length);
}

static int qsfp_require_module(const struct device *bus, uint8_t cage, uint8_t *pins)
{
	int ret = i2c_reg_read_byte(bus, qsfp_cages[cage].expander_addr, TCA9554_REG_INPUT, pins);

	if (ret != 0) {
		/*
		 * An unreachable expander hides a hot-swap: the module behind it
		 * may be replaced before the periodic poll notices. Drop the DOM
		 * caps so the next DOM_LANE re-reads them from the module.
		 */
		qsfp_dom_cache_invalidate(cage);
		return QSFP_MGMT_ERR_I2C;
	}
	if ((*pins & QSFP_BIT_MODPRSL) != 0) {
		qsfp_dom_cache_invalidate(cage);
		return QSFP_MGMT_ERR_ABSENT;
	}
	return QSFP_MGMT_OK;
}

static int qsfp_mgmt_status(const struct device *bus, uint8_t cage,
			    struct qsfp_mgmt_response *response)
{
	struct qsfp_status_payload data = {0};
	int ret = qsfp_require_module(bus, cage, &data.pins);

	data.power_mode =
		(qsfp_output_shadow[cage] & QSFP_BIT_LPMODE) ? QSFP_POWER_LOW : QSFP_POWER_HIGH;
	if (ret == QSFP_MGMT_ERR_ABSENT) {
		qsfp_response_copy(response, &data, sizeof(data));
		return ret;
	}
	if (ret != 0 || qsfp_select(bus, cage) != 0) {
		qsfp_dom_cache_invalidate(cage);
		return QSFP_MGMT_ERR_I2C;
	}
	k_msleep(1);
	if (qsfp_cmis_read_identity(bus, &data.identifier, &data.revision) != 0) {
		qsfp_dom_cache_invalidate(cage);
		ret = QSFP_MGMT_ERR_I2C;
	} else {
		uint8_t state;

		/*
		 * Host STATUS can observe a newly seated or replaced module before
		 * the periodic poll runs. Drop cached DOM caps so the next DOM_*
		 * call re-reads this module's monitor capabilities.
		 */
		qsfp_dom_cache_invalidate(cage);
		ret = QSFP_MGMT_OK;
		if (qsfp_ident_cmis(data.identifier)) {
			if (i2c_reg_read_byte(bus, QSFP_MODULE_I2C_ADDR, CMIS_MODULE_STATE_OFF,
					      &state) != 0) {
				ret = QSFP_MGMT_ERR_I2C;
			} else {
				data.module_state = (state & CMIS_MODULE_STATE_MASK) >> 1;
			}
		}
	}
	qsfp_deselect(bus, cage);
	qsfp_response_copy(response, &data, sizeof(data));
	return ret;
}

static int qsfp_mgmt_inventory(const struct device *bus, uint8_t cage, uint8_t field,
			       struct qsfp_mgmt_response *response)
{
	uint8_t lower[2];
	uint8_t upper[128];
	uint8_t pins;
	const uint8_t *src = NULL;
	size_t len = 0;
	int ret;

	if (field >= QSFP_INV_COUNT) {
		return QSFP_MGMT_ERR_ARGUMENT;
	}
	ret = qsfp_require_module(bus, cage, &pins);
	if (ret != 0) {
		return ret;
	}
	if (qsfp_select(bus, cage) != 0) {
		return QSFP_MGMT_ERR_I2C;
	}
	k_msleep(1);

	if (field == QSFP_INV_IDENTIFIER) {
		if (qsfp_cmis_read(bus, 0, lower, sizeof(lower)) == 0) {
			src = lower;
			len = sizeof(lower);
		}
	} else if (field == QSFP_INV_MEDIA_TYPE) {
		if (qsfp_cmis_read(bus, CMIS_MEDIA_TYPE_OFF, lower, 1) == 0) {
			src = lower;
			len = 1;
		}
	} else if (field == QSFP_INV_APPLICATIONS || field == QSFP_INV_APP_LANES) {
		uint8_t descriptors[CMIS_APP_DESC_COUNT * CMIS_APP_DESC_LEN];
		uint8_t packed[CMIS_APP_DESC_COUNT * 2];
		uint8_t half = (field == QSFP_INV_APPLICATIONS) ? 0U : 2U;
		uint8_t i;

		if (qsfp_cmis_read(bus, CMIS_APP_DESC_OFF, descriptors, sizeof(descriptors)) == 0) {
			for (i = 0; i < CMIS_APP_DESC_COUNT; i++) {
				packed[2U * i] = descriptors[CMIS_APP_DESC_LEN * i + half];
				packed[2U * i + 1U] =
					descriptors[CMIS_APP_DESC_LEN * i + half + 1U];
			}
			memcpy(upper, packed, sizeof(packed));
			src = upper;
			len = sizeof(packed);
		}
	} else if (qsfp_cmis_read_page(bus, 0x00, upper) == 0) {
		switch (field) {
		case QSFP_INV_VENDOR_NAME:
			src = &upper[CMIS_P00_VENDOR_NAME_OFF];
			len = CMIS_P00_VENDOR_NAME_LEN;
			break;
		case QSFP_INV_VENDOR_OUI:
			src = &upper[CMIS_P00_VENDOR_OUI_OFF];
			len = 3;
			break;
		case QSFP_INV_VENDOR_PN:
			src = &upper[CMIS_P00_VENDOR_PN_OFF];
			len = CMIS_P00_VENDOR_PN_LEN;
			break;
		case QSFP_INV_VENDOR_REV:
			src = &upper[CMIS_P00_VENDOR_REV_OFF];
			len = CMIS_P00_VENDOR_REV_LEN;
			break;
		case QSFP_INV_VENDOR_SN:
			src = &upper[CMIS_P00_VENDOR_SN_OFF];
			len = CMIS_P00_VENDOR_SN_LEN;
			break;
		case QSFP_INV_DATE_CODE:
			src = &upper[CMIS_P00_DATE_OFF];
			len = CMIS_P00_DATE_LEN;
			break;
		case QSFP_INV_CONNECTOR:
			src = &upper[CMIS_P00_CONNECTOR_OFF];
			len = 1;
			break;
		default:
			break;
		}
	}

	if (src != NULL) {
		if (field == QSFP_INV_VENDOR_NAME || field == QSFP_INV_VENDOR_PN ||
		    field == QSFP_INV_VENDOR_REV || field == QSFP_INV_VENDOR_SN ||
		    field == QSFP_INV_DATE_CODE) {
			while (len > 0 && src[len - 1] == ' ') {
				len--;
			}
		}
		qsfp_response_copy(response, src, len);
		ret = QSFP_MGMT_OK;
	} else {
		ret = QSFP_MGMT_ERR_I2C;
	}
	(void)i2c_reg_write_byte(bus, QSFP_MODULE_I2C_ADDR, CMIS_REG_PAGE_SELECT, 0);
	qsfp_deselect(bus, cage);
	return ret;
}

static int qsfp_mgmt_set_power(const struct device *bus, uint8_t cage, uint8_t mode)
{
	uint8_t output;

	if (mode == QSFP_POWER_LOW) {
		output = qsfp_output_shadow[cage] | QSFP_BIT_LPMODE;
	} else if (mode == QSFP_POWER_HIGH) {
		output = qsfp_output_shadow[cage] & ~QSFP_BIT_LPMODE;
	} else {
		return QSFP_MGMT_ERR_ARGUMENT;
	}

	/*
	 * LPMODE is an expander pin. An empty cage is still valid: the next
	 * module sees the parked level. Optical modules can spend several
	 * seconds in PwrUp; the host polls STATUS for CMIS Ready.
	 */
	if (qsfp_write_output(bus, cage, output) != 0) {
		(void)qsfp_recover(bus);
		return QSFP_MGMT_ERR_I2C;
	}
	return QSFP_MGMT_OK;
}

static int qsfp_mgmt_reset(const struct device *bus, uint8_t cage)
{
	uint8_t pins;
	uint8_t saved = qsfp_output_shadow[cage] | QSFP_BIT_MODSELL;
	int ret = qsfp_require_module(bus, cage, &pins);

	if (ret != 0) {
		return ret;
	}
	if (qsfp_write_output(bus, cage, saved & ~QSFP_BIT_RESETL) != 0) {
		return QSFP_MGMT_ERR_I2C;
	}
	k_msleep(10);
	if (qsfp_write_output(bus, cage, saved | QSFP_BIT_RESETL) != 0) {
		qsfp_output_shadow[cage] = saved | QSFP_BIT_RESETL;
		return QSFP_MGMT_ERR_I2C;
	}
	qsfp_dom_cache_invalidate(cage);
	return QSFP_MGMT_OK;
}

static int qsfp_mgmt_dom_module(const struct device *bus, uint8_t cage,
				struct qsfp_mgmt_response *response)
{
	struct qsfp_dom_module dom = {0};
	uint8_t lower[28];
	uint8_t pins;
	int ret = qsfp_require_module(bus, cage, &pins);

	if (ret != 0 || qsfp_select(bus, cage) != 0) {
		return ret != 0 ? ret : QSFP_MGMT_ERR_I2C;
	}
	k_msleep(1);
	ret = qsfp_cmis_read(bus, 0, lower, sizeof(lower));
	qsfp_deselect(bus, cage);
	if (ret != 0) {
		return QSFP_MGMT_ERR_I2C;
	}
	if (qsfp_ident_sff8636(lower[0])) {
		/* SFF-8636: temp @22-23, Vcc @26-27 (CMIS uses 14-17). */
		dom.temperature_256c = (int16_t)sys_get_be16(&lower[22]);
		dom.vcc_100uv = sys_get_be16(&lower[26]);
		dom.module_state = 0;
	} else if (qsfp_ident_cmis(lower[0])) {
		dom.temperature_256c = (int16_t)sys_get_be16(&lower[14]);
		dom.vcc_100uv = sys_get_be16(&lower[16]);
		dom.module_state = (lower[3] & CMIS_MODULE_STATE_MASK) >> 1;
	} else {
		return QSFP_MGMT_ERR_UNAVAILABLE;
	}
	dom.low_power = (qsfp_output_shadow[cage] & QSFP_BIT_LPMODE) != 0;
	dom.interrupt_asserted = (pins & QSFP_BIT_INTL) == 0;
	qsfp_response_copy(response, &dom, sizeof(dom));
	if (dom.temperature_256c == 0 && dom.vcc_100uv == 0) {
		return QSFP_MGMT_ERR_UNAVAILABLE;
	}
	return QSFP_MGMT_OK;
}

static int qsfp_mgmt_dom_lane(const struct device *bus, uint8_t cage, uint8_t lane,
			      struct qsfp_mgmt_response *response)
{
	struct qsfp_dom_lane dom;
	uint8_t upper[128];
	uint8_t pins;
	int ret;

	if (lane >= 8) {
		return QSFP_MGMT_ERR_ARGUMENT;
	}
	ret = qsfp_require_module(bus, cage, &pins);
	if (ret != 0) {
		return ret;
	}
	if ((qsfp_output_shadow[cage] & QSFP_BIT_LPMODE) != 0) {
		return QSFP_MGMT_ERR_POWER;
	}
	if (qsfp_select(bus, cage) != 0) {
		return QSFP_MGMT_ERR_I2C;
	}
	k_msleep(1);
	if (dom_bias_multiplier[cage] == 0) {
		int cap = qsfp_cmis_read_page(bus, 0x01, upper);

		if (cap != 0) {
			qsfp_deselect(bus, cage);
			return qsfp_page_status(cap);
		}
		dom_monitor_flags[cage] = upper[CMIS_P01_MONITOR_CAP_OFF] & GENMASK(2, 0);
		dom_bias_multiplier[cage] = 1U << ((upper[CMIS_P01_MONITOR_CAP_OFF] >> 3) & 0x3);
	}
	ret = qsfp_cmis_read_page(bus, 0x11, upper);
	(void)i2c_reg_write_byte(bus, QSFP_MODULE_I2C_ADDR, CMIS_REG_PAGE_SELECT, 0);
	qsfp_deselect(bus, cage);
	if (ret != 0) {
		return qsfp_page_status(ret);
	}
	dom.tx_power_01uw = sys_get_be16(&upper[CMIS_P11_TX_POWER_OFF + lane * 2]);
	dom.tx_bias_2ua = sys_get_be16(&upper[CMIS_P11_TX_BIAS_OFF + lane * 2]);
	dom.rx_power_01uw = sys_get_be16(&upper[CMIS_P11_RX_POWER_OFF + lane * 2]);
	dom.tx_bias_multiplier = dom_bias_multiplier[cage];
	dom.monitor_flags = dom_monitor_flags[cage];
	qsfp_response_copy(response, &dom, sizeof(dom));
	return QSFP_MGMT_OK;
}

static int qsfp_mgmt_read_page(const struct device *bus, uint8_t cage, uint8_t arg,
			       struct qsfp_mgmt_response *response)
{
	static const uint8_t page_numbers[QSFP_MGMT_PAGE_COUNT] = {0x00, 0x01, 0x02,
								   0x10, 0x11, 0x00};
	uint8_t index = QSFP_MGMT_PAGE_ARG_INDEX(arg);
	uint8_t block = QSFP_MGMT_PAGE_ARG_BLOCK(arg);
	size_t offset = (size_t)block * QSFP_MGMT_PAYLOAD_SIZE;
	uint8_t data[128];
	uint8_t pins;
	int ret;

	if (index >= QSFP_MGMT_PAGE_COUNT || offset >= sizeof(data)) {
		return QSFP_MGMT_ERR_ARGUMENT;
	}
	ret = qsfp_require_module(bus, cage, &pins);
	if (ret != 0) {
		return ret;
	}
	if (qsfp_select(bus, cage) != 0) {
		return QSFP_MGMT_ERR_I2C;
	}
	k_msleep(1);
	if (index == QSFP_MGMT_PAGE_LOWER) {
		ret = qsfp_cmis_read(bus, 0, data, sizeof(data));
	} else {
		ret = qsfp_cmis_read_page(bus, page_numbers[index], data);
		(void)i2c_reg_write_byte(bus, QSFP_MODULE_I2C_ADDR, CMIS_REG_PAGE_SELECT, 0);
	}
	qsfp_deselect(bus, cage);
	if (ret != 0) {
		return qsfp_page_status(ret);
	}
	qsfp_response_copy(response, &data[offset],
			   MIN((size_t)QSFP_MGMT_PAYLOAD_SIZE, sizeof(data) - offset));
	return QSFP_MGMT_OK;
}

void qsfp_handle_mgmt(uint32_t request, struct qsfp_mgmt_response *response)
{
	const struct device *bus;
	uint8_t op = QSFP_MGMT_REQUEST_OP(request);
	uint8_t cage = QSFP_MGMT_REQUEST_CAGE(request);
	uint8_t arg = QSFP_MGMT_REQUEST_ARG(request);
	int status;

	qsfp_response_init(request, response);
	if (op >= QSFP_MGMT_OP_COUNT || cage >= ARRAY_SIZE(qsfp_cages)) {
		response->status = QSFP_MGMT_ERR_ARGUMENT;
		return;
	}

	bus = qsfp_session_begin();
	if (bus == NULL) {
		response->status = QSFP_MGMT_ERR_I2C;
		return;
	}

	switch (op) {
	case QSFP_MGMT_OP_STATUS:
		status = qsfp_mgmt_status(bus, cage, response);
		break;
	case QSFP_MGMT_OP_INVENTORY:
		status = qsfp_mgmt_inventory(bus, cage, arg, response);
		break;
	case QSFP_MGMT_OP_SET_POWER:
		status = qsfp_mgmt_set_power(bus, cage, arg);
		break;
	case QSFP_MGMT_OP_RESET:
		status = qsfp_mgmt_reset(bus, cage);
		break;
	case QSFP_MGMT_OP_DOM_MODULE:
		status = qsfp_mgmt_dom_module(bus, cage, response);
		break;
	case QSFP_MGMT_OP_DOM_LANE:
		status = qsfp_mgmt_dom_lane(bus, cage, arg, response);
		break;
	case QSFP_MGMT_OP_READ_PAGE:
		status = qsfp_mgmt_read_page(bus, cage, arg, response);
		break;
	default:
		status = QSFP_MGMT_ERR_ARGUMENT;
		break;
	}

	response->status = status;
	qsfp_session_end(bus);
}
