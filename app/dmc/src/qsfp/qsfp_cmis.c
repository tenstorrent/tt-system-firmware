/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared CMIS access helpers for QSFP-DD modules.
 */

#include "qsfp_cmis.h"

#include <errno.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

int qsfp_cmis_read(const struct device *bus, uint8_t offset, uint8_t *buf, size_t len)
{
	size_t got = 0;

	while (got < len) {
		uint8_t start = (uint8_t)(offset + got);
		size_t chunk = MIN(8U, len - got);
		int ret = i2c_write_read(bus, QSFP_MODULE_I2C_ADDR, &start, 1, &buf[got], chunk);

		if (ret != 0) {
			(void)i2c_recover_bus(bus);
			return ret;
		}
		got += chunk;
	}
	return 0;
}

bool qsfp_cmis_flat_mem(const struct device *bus)
{
	uint8_t ident;
	uint8_t status;

	if (i2c_reg_read_byte(bus, QSFP_MODULE_I2C_ADDR, CMIS_REG_IDENTIFIER, &ident) != 0 ||
	    i2c_reg_read_byte(bus, QSFP_MODULE_I2C_ADDR, CMIS_REG_STATUS, &status) != 0) {
		return false;
	}
	if (qsfp_ident_sff8636(ident)) {
		return (status & SFF8636_STATUS_FLAT_MEM) != 0;
	}
	return (status & CMIS_STATUS_FLAT_MEM) != 0;
}

int qsfp_cmis_read_page(const struct device *bus, uint8_t page, uint8_t *upper)
{
	uint8_t ident = 0;
	bool flat;
	int tries;

	if (i2c_reg_read_byte(bus, QSFP_MODULE_I2C_ADDR, CMIS_REG_IDENTIFIER, &ident) != 0) {
		return -EIO;
	}
	/*
	 * CMIS pages 01h/02h/10h/11h are not SFF-8636 pages. Flat-memory CMIS
	 * modules implement page 00h only; some reject page-select writes.
	 */
	if (page != 0 && qsfp_ident_sff8636(ident)) {
		return -ENOTSUP;
	}
	flat = qsfp_cmis_flat_mem(bus);
	if (flat && page != 0) {
		return -ENOTSUP;
	}

	for (tries = 0; tries < 3; tries++) {
		if (!flat && i2c_reg_write_byte(bus, QSFP_MODULE_I2C_ADDR, CMIS_REG_PAGE_SELECT,
						page) != 0) {
			k_msleep(5);
			continue;
		}
		k_msleep(5);
		if (qsfp_cmis_read(bus, 128, upper, 128) == 0) {
			return 0;
		}
		k_msleep(5);
	}
	return -EIO;
}

int qsfp_cmis_read_identity(const struct device *bus, uint8_t *identifier, uint8_t *revision)
{
	int ret;

	ret = i2c_reg_read_byte(bus, QSFP_MODULE_I2C_ADDR, CMIS_REG_IDENTIFIER, identifier);
	if (ret != 0) {
		return ret;
	}
	return i2c_reg_read_byte(bus, QSFP_MODULE_I2C_ADDR, CMIS_REG_REVISION, revision);
}

const char *qsfp_cmis_identifier_str(uint8_t identifier)
{
	switch (identifier) {
	case 0x0c:
		return "QSFP";
	case 0x0d:
		return "QSFP+";
	case 0x11:
		return "QSFP28";
	case 0x18:
		return "QSFP-DD (CMIS)";
	case 0x19:
		return "OSFP (CMIS)";
	case 0x1e:
		return "QSFP+ w/ CMIS";
	default:
		return "unknown";
	}
}
