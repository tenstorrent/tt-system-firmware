/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QSFP-DD cage discovery and periodic presence telemetry.
 */

#include "qsfp.h"
#include "qsfp_bus.h"
#include "qsfp_cmis.h"

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(qsfp_telemetry, CONFIG_TT_APP_LOG_LEVEL);

static uint32_t qsfp_probe(bool log_discovery)
{
	const struct device *bus = qsfp_session_begin();
	uint32_t status = 0;
	uint8_t responsive = 0;

	if (bus == NULL) {
		LOG_ERR("QSFP: i2c3 (MCU_I2C0) not ready, skipping discovery");
		return status;
	}

	ARRAY_FOR_EACH(qsfp_cages, i) {
		const struct qsfp_cage *cage = &qsfp_cages[i];
		uint8_t pins;
		uint8_t identifier = 0;
		uint8_t revision = 0;
		int ident_ret;

		if (i2c_reg_read_byte(bus, cage->expander_addr, TCA9554_REG_INPUT, &pins) != 0) {
			qsfp_dom_cache_invalidate(i);
			if (log_discovery) {
				LOG_WRN("QSFP %s: expander 0x%02x absent", cage->name,
					cage->expander_addr);
			}
			continue;
		}
		responsive++;

		if ((pins & QSFP_BIT_MODPRSL) != 0) {
			qsfp_dom_cache_invalidate(i);
			status |= (uint32_t)QSFP_TELEM_NO_MODULE << (8 * i);
			if (log_discovery) {
				LOG_INF("QSFP %s: expander 0x%02x present, no module", cage->name,
					cage->expander_addr);
			}
			continue;
		}

		if (qsfp_select(bus, i) != 0) {
			qsfp_deselect(bus, i);
			qsfp_dom_cache_invalidate(i);
			status |= (uint32_t)QSFP_TELEM_CMIS_READ_FAILED << (8 * i);
			if (log_discovery) {
				LOG_WRN("QSFP %s: module seated, select failed", cage->name);
			}
			continue;
		}
		k_msleep(1);
		ident_ret = qsfp_cmis_read_identity(bus, &identifier, &revision);
		qsfp_deselect(bus, i);

		if (ident_ret != 0) {
			qsfp_dom_cache_invalidate(i);
			status |= (uint32_t)QSFP_TELEM_CMIS_READ_FAILED << (8 * i);
			if (log_discovery) {
				LOG_WRN("QSFP %s: module seated, identifier read failed",
					cage->name);
			}
			continue;
		}

		/*
		 * Many CMIS modules share identifier 0x18. Always drop the DOM
		 * capability cache when a present module is observed so a
		 * same-id hot-swap cannot reuse the previous bias/monitor flags.
		 */
		qsfp_dom_cache_invalidate(i);

		status |= (uint32_t)QSFP_TELEM_PRESENT_ID(identifier) << (8 * i);
		if (log_discovery) {
			if (qsfp_ident_sff8636(identifier)) {
				LOG_INF("QSFP %s: %s id=0x%02x, SFF-8636 rev 0x%02x", cage->name,
					qsfp_cmis_identifier_str(identifier), identifier, revision);
			} else {
				LOG_INF("QSFP %s: %s id=0x%02x, CMIS %u.%u", cage->name,
					qsfp_cmis_identifier_str(identifier), identifier,
					revision >> 4, revision & 0x0f);
			}
		}
	}

	if (responsive == 0 && qsfp_recover(bus) != 0) {
		status = (uint32_t)QSFP_TELEM_BUS_STUCK * 0x01010101U;
	}
	qsfp_session_end(bus);
	return status;
}

uint32_t qsfp_discover(void)
{
	return qsfp_probe(true);
}

uint32_t qsfp_poll(void)
{
	return qsfp_probe(false);
}
