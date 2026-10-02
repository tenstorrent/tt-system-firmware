/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Module EEPROM at 0x50 (CMIS paging; SFF-8636 identity/flat-mem checks).
 */

#ifndef APP_DMC_QSFP_CMIS_H_
#define APP_DMC_QSFP_CMIS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#define QSFP_MODULE_I2C_ADDR 0x50
#define CMIS_REG_IDENTIFIER  0x00
#define CMIS_REG_REVISION    0x01
#define CMIS_REG_STATUS      0x02
#define CMIS_REG_PAGE_SELECT 0x7f

#define CMIS_STATUS_FLAT_MEM    BIT(7)
#define SFF8636_STATUS_FLAT_MEM BIT(2)

static inline bool qsfp_ident_sff8636(uint8_t identifier)
{
	return identifier == 0x0c || identifier == 0x0d || identifier == 0x11;
}

static inline bool qsfp_ident_cmis(uint8_t identifier)
{
	return identifier == 0x18 || identifier == 0x19 || identifier == 0x1e;
}

#define CMIS_P00_VENDOR_NAME_OFF 1
#define CMIS_P00_VENDOR_NAME_LEN 16
#define CMIS_P00_VENDOR_OUI_OFF  17
#define CMIS_P00_VENDOR_PN_OFF   20
#define CMIS_P00_VENDOR_PN_LEN   16
#define CMIS_P00_VENDOR_REV_OFF  36
#define CMIS_P00_VENDOR_REV_LEN  2
#define CMIS_P00_VENDOR_SN_OFF   38
#define CMIS_P00_VENDOR_SN_LEN   16
#define CMIS_P00_DATE_OFF        54
#define CMIS_P00_DATE_LEN        8
#define CMIS_P00_CONNECTOR_OFF   75
/* Media Type Encoding is lower memory; upper page 00h byte 0 is the Identifier. */
#define CMIS_MEDIA_TYPE_OFF      85
#define CMIS_APP_DESC_OFF        86
#define CMIS_APP_DESC_COUNT      8
#define CMIS_APP_DESC_LEN        4

#define CMIS_MODULE_STATE_OFF    3
#define CMIS_MODULE_STATE_MASK   GENMASK(3, 1)
#define CMIS_P11_TX_POWER_OFF    26
#define CMIS_P11_TX_BIAS_OFF     42
#define CMIS_P11_RX_POWER_OFF    58
#define CMIS_P01_MONITOR_CAP_OFF 32

int qsfp_cmis_read(const struct device *bus, uint8_t offset, uint8_t *buf, size_t len);
int qsfp_cmis_read_page(const struct device *bus, uint8_t page, uint8_t *upper);
bool qsfp_cmis_flat_mem(const struct device *bus);
int qsfp_cmis_read_identity(const struct device *bus, uint8_t *identifier, uint8_t *revision);
const char *qsfp_cmis_identifier_str(uint8_t identifier);

#endif /* APP_DMC_QSFP_CMIS_H_ */
