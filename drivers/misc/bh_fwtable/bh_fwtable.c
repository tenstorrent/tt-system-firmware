/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_bh_fwtable

#include <stddef.h>

#include <pb_decode.h>
#include <tenstorrent/tt_boot_fs.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/misc/bh_fwtable.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

#ifdef CONFIG_BH_FWTABLE_CCFGOVR
#include "ccfgovr.h"
#include "fw_table_override.pb.h"

#define CCFGOVR_DECODE_BODY_MAX 512U
BUILD_ASSERT(CCFGOVR_DECODE_BODY_MAX <= CCFGOVR_MAX_BODY_LEN,
	     "ccfgovr decode cap cannot exceed physical body capacity");
#endif

#define RESET_UNIT_STRAP_REGISTERS_L_REG_ADDR 0x80030D20

LOG_MODULE_REGISTER(bh_fwtable, CONFIG_BH_FWTABLE_LOG_LEVEL);

enum bh_fwtable_e {
	BH_FWTABLE_FLSHINFO,
	BH_FWTABLE_BOARDCFG,
	BH_FWTABLE_CMFWCFG,
};

struct bh_fwtable_config {
	const struct device *flash;
};

struct bh_fwtable_data {
	FwTable fw_table;
	FlashInfoTable flash_info_table;
	ReadOnly read_only_table;
};

/* Getter function that returns a const pointer to the fw table */
const FwTable *tt_bh_fwtable_get_fw_table(const struct device *dev)
{
	struct bh_fwtable_data *data = dev->data;

	if (!device_is_ready(dev) && !IS_ENABLED(CONFIG_TT_SMC_RECOVERY)) {
		LOG_DBG("%s table has not been loaded", "Firmware");
	}
	return &data->fw_table;
}

const FlashInfoTable *tt_bh_fwtable_get_flash_info_table(const struct device *dev)
{
	struct bh_fwtable_data *data = dev->data;

	if (!device_is_ready(dev) && !IS_ENABLED(CONFIG_TT_SMC_RECOVERY)) {
		LOG_DBG("%s table has not been loaded", "Flash Info");
	}
	return &data->flash_info_table;
}

const ReadOnly *tt_bh_fwtable_get_read_only_table(const struct device *dev)
{
	struct bh_fwtable_data *data = dev->data;

	if (!device_is_ready(dev) && !IS_ENABLED(CONFIG_TT_SMC_RECOVERY)) {
		LOG_DBG("%s table has not been loaded", "Read Only");
	}
	return &data->read_only_table;
}

/* Converts a board id extracted from board type and converts it to a PCB Type */
PcbType tt_bh_fwtable_get_pcb_type(const struct device *dev)
{
	PcbType pcb_type;
	struct bh_fwtable_data *data = dev->data;

	if (!device_is_ready(dev)) {
		return PcbTypeUnknown;
	}

	/* Extract board type from board_id */
	uint8_t board_type = (uint8_t)((data->read_only_table.board_id >> 36) & 0xFF);

	/* Figure out PCB type from board type */
	switch (board_type) {
	case BOARDTYPE_ORION_SLT:
		pcb_type = PcbTypeOrionSLT;
		break;
	/* Note: the P100A is a depopulated P150, so PcbType is actually P150 */
	/* eth will be all disabled as per P100 specs anyways */
	case BOARDTYPE_P100A:
	case BOARDTYPE_P150:
	case BOARDTYPE_P150A:
	case BOARDTYPE_P150C:
		pcb_type = PcbTypeP150;
		break;
	case BOARDTYPE_P300:
	case BOARDTYPE_P300A:
	case BOARDTYPE_P300C:
		pcb_type = PcbTypeP300;
		break;
	case BOARDTYPE_UBB:
		pcb_type = PcbTypeUBB;
		break;
	default:
		pcb_type = PcbTypeUnknown;
		break;
	}

	return pcb_type;
}

/* Returns the board type extracted from board_id (bits 36-43) */
uint8_t tt_bh_fwtable_get_board_type(const struct device *dev)
{
	struct bh_fwtable_data *data = dev->data;

	if (!device_is_ready(dev)) {
		return 0xFF;
	}

	/* Extract board type from board_id */
	return (uint8_t)((data->read_only_table.board_id >> 36) & 0xFF);
}

/* Reads GPIO6 to determine whether it is p300 left chip. GPIO6 is only set on p300 left chip. */
bool tt_bh_fwtable_is_p300_left_chip(void)
{
	return FIELD_GET(BIT(6), sys_read32(RESET_UNIT_STRAP_REGISTERS_L_REG_ADDR));
}

uint32_t tt_bh_fwtable_get_asic_location(const struct device *dev)
{
	struct bh_fwtable_data *data = dev->data;

	if (!device_is_ready(dev)) {
		LOG_DBG("device is not ready");
		return 0;
	}

	if (tt_bh_fwtable_get_pcb_type(dev) == PcbTypeP300) {
		/* For the p300 a value of 1 is the left asic and 0 is the right */
		return tt_bh_fwtable_is_p300_left_chip();
	} else if (tt_bh_fwtable_get_pcb_type(dev) == PcbTypeUBB) {
		/* For the UBB asic location is needed to determine training modes and should be
		 * populated in SPI
		 */
		return data->read_only_table.asic_location;
	}

	/* For all other supported boards this value is 0 */
	return 0;
}

/* Loader function that deserializes the fw table bin from the SPI filesystem */
static int tt_bh_fwtable_load(const struct device *dev, enum bh_fwtable_e table)
{
#define BH_FWTABLE_LOADCFG(_enum, _tag, _field, _msgtype)                                          \
	[BH_FWTABLE_##_enum] = {                                                                   \
		.tag = #_tag,                                                                      \
		.offs = offsetof(struct bh_fwtable_data, _field),                                  \
		.size = sizeof(((struct bh_fwtable_data *)0)->_field),                             \
		.msg = &_msgtype##_msg,                                                            \
	}

	uint8_t buffer[256];
	size_t bytes_read = 0;
	struct bh_fwtable_data *data = dev->data;
	const struct bh_fwtable_config *config = dev->config;
	static const struct loadcfg {
		const char *tag;
		size_t offs;             /* field offset within the bh_fwtable_data struct */
		size_t size;             /* field size within the bh_fwtable_data struct */
		const pb_msgdesc_t *msg; /* pointer to protobuf message */
	} loadcfg[] = {
		BH_FWTABLE_LOADCFG(FLSHINFO, flshinfo, flash_info_table, FlashInfoTable),
		BH_FWTABLE_LOADCFG(BOARDCFG, boardcfg, read_only_table, ReadOnly),
		BH_FWTABLE_LOADCFG(CMFWCFG, cmfwcfg, fw_table, FwTable),
	};

	__ASSERT_NO_MSG(table < ARRAY_SIZE(loadcfg));

	tt_boot_fs_fd fd_data;
	int result =
		tt_boot_fs_find_fd_by_tag(config->flash, (uint8_t *)loadcfg[table].tag, &fd_data);
	if (result != TT_BOOT_FS_OK) {
		LOG_ERR("%8s() failed with error code %d", loadcfg[table].tag, result);
		return -EIO;
	}

	bytes_read = fd_data.flags.f.image_size;

	if (bytes_read > sizeof(buffer)) {
		LOG_ERR("Buffer is too small for %8s", loadcfg[table].tag);
		return -ENOMEM;
	}

	flash_read(config->flash, fd_data.spi_addr, buffer, bytes_read);
	/* Convert the binary data to a pb_istream_t that is expected by decode */
	pb_istream_t stream = pb_istream_from_buffer(buffer, bytes_read);
	/* PB_DECODE_NULLTERMINATED: Expect the message to be terminated with zero tag */
	if (!pb_decode_ex(&stream, loadcfg[table].msg, (uint8_t *)data + loadcfg[table].offs,
			  PB_DECODE_NULLTERMINATED)) {
		LOG_ERR("%s() failed: '%s'", "pb_decode_ex", loadcfg[table].tag);
		return -EINVAL;
	}

	LOG_DBG("Loaded %s", loadcfg[table].tag);
	return 0;
}

#ifdef CONFIG_BH_FWTABLE_CCFGOVR

struct ccfgovr_bank_info {
	const char *tag;
	tt_boot_fs_fd fd;
	struct ccfgovr_bank_hdr hdr;
	bool header_valid;
};

static bool ccfgovr_header_is_plausible(const struct ccfgovr_bank_hdr *hdr)
{
	return hdr->magic == CCFGOVR_MAGIC && hdr->seq != CCFGOVR_SEQ_ERASED &&
	       hdr->version == CCFGOVR_HDR_VERSION && (hdr->body_len % sizeof(uint32_t)) == 0 &&
	       hdr->body_len <= CCFGOVR_DECODE_BODY_MAX;
}

static void ccfgovr_read_header(const struct device *flash, struct ccfgovr_bank_info *info)
{
	int rc;

	info->header_valid = false;

	rc = tt_boot_fs_find_fd_by_tag(flash, (const uint8_t *)info->tag, &info->fd);
	if (rc != TT_BOOT_FS_OK) {
		LOG_DBG("ccfgovr bank '%s' has no boot-fs entry (rc=%d)", info->tag, rc);
		return;
	}

	if (info->fd.flags.f.image_size < sizeof(info->hdr)) {
		LOG_WRN("ccfgovr bank '%s' image_size=%u smaller than header", info->tag,
			info->fd.flags.f.image_size);
		return;
	}

	rc = flash_read(flash, info->fd.spi_addr, &info->hdr, sizeof(info->hdr));
	if (rc < 0) {
		LOG_WRN("flash_read(%s hdr) failed: %d", info->tag, rc);
		return;
	}

	info->header_valid = ccfgovr_header_is_plausible(&info->hdr);
	if (!info->header_valid) {
		LOG_DBG("ccfgovr bank '%s' header rejected (magic=0x%08x seq=0x%08x "
			"body_len=%u version=%u)",
			info->tag, info->hdr.magic, info->hdr.seq, info->hdr.body_len,
			info->hdr.version);
	}
}

static bool ccfgovr_load_and_verify_body(const struct device *flash,
					 const struct ccfgovr_bank_info *info, uint8_t *body,
					 size_t body_cap)
{
	uint32_t crc;
	int rc;

	if (info->hdr.body_len > body_cap) {
		/* Already bounded by ccfgovr_header_is_plausible(); defensive. */
		return false;
	}

	if (info->hdr.body_len > 0) {
		rc = flash_read(flash, info->fd.spi_addr + sizeof(info->hdr), body,
				info->hdr.body_len);
		if (rc < 0) {
			LOG_WRN("flash_read(%s body) failed: %d", info->tag, rc);
			return false;
		}
	}

	crc = crc32_ieee_update(0, (const uint8_t *)&info->hdr,
				offsetof(struct ccfgovr_bank_hdr, cksum));
	crc = crc32_ieee_update(crc, body, info->hdr.body_len);
	if (crc != info->hdr.cksum) {
		LOG_WRN("ccfgovr bank '%s' CRC mismatch: got 0x%08x expected 0x%08x", info->tag,
			crc, info->hdr.cksum);
		return false;
	}

	return true;
}

static bool ccfgovr_seq_is_newer(uint32_t a, uint32_t b)
{
	return ((int32_t)(a - b)) > 0;
}

void tt_bh_fwtable_apply_ccfgovr(const struct device *dev)
{
	struct bh_fwtable_data *data = dev->data;
	const struct bh_fwtable_config *config = dev->config;
	struct ccfgovr_bank_info banks[2] = {
		{.tag = CCFGOVR_TAG_A},
		{.tag = CCFGOVR_TAG_B},
	};
	uint8_t body[CCFGOVR_DECODE_BODY_MAX];

	ccfgovr_read_header(config->flash, &banks[0]);
	ccfgovr_read_header(config->flash, &banks[1]);

	/*
	 * Build an ordered list of plausible candidates with newest seq first and
	 * try them sequentially.
	 */
	struct ccfgovr_bank_info *order[2] = {NULL, NULL};
	size_t n_candidates = 0;

	for (size_t i = 0; i < ARRAY_SIZE(banks); i++) {
		if (banks[i].header_valid) {
			order[n_candidates++] = &banks[i];
		}
	}

	if (n_candidates == 2 && ccfgovr_seq_is_newer(order[1]->hdr.seq, order[0]->hdr.seq)) {
		struct ccfgovr_bank_info *tmp = order[0];

		order[0] = order[1];
		order[1] = tmp;
	}

	for (size_t i = 0; i < n_candidates; i++) {
		if (!ccfgovr_load_and_verify_body(config->flash, order[i], body, sizeof(body))) {
			continue;
		}

		LOG_INF("Applying CCFGOVR bank '%s' seq=%u (%u override bytes)", order[i]->tag,
			order[i]->hdr.seq, order[i]->hdr.body_len);

		/* Valid empty override; nothing to decode. */
		if (order[i]->hdr.body_len == 0) {
			return;
		}

		/*
		 * Tags that are `reserved` in fw_table_override.proto are unknown
		 * to FwTableOverride_fields and get silently skipped.
		 */
		FwTableOverride ovr = FwTableOverride_init_zero;
		pb_istream_t stream = pb_istream_from_buffer(body, order[i]->hdr.body_len);

		if (!pb_decode_ex(&stream, FwTableOverride_fields, &ovr,
				  PB_DECODE_NULLTERMINATED)) {
			LOG_WRN("ccfgovr bank '%s' protobuf decode failed; trying next bank",
				order[i]->tag);
			continue;
		}

		/*
		 * Allow-listed merge. Add one branch here for each field exposed
		 * in fw_table_override.proto; keep this list in sync with the
		 * `optional` declarations there.
		 */
		if (ovr.has_chip_limits && ovr.chip_limits.has_tdp_limit) {
			LOG_INF("CCFGOVR override: chip_limits.tdp_limit = %u",
				ovr.chip_limits.tdp_limit);
			data->fw_table.chip_limits.tdp_limit = ovr.chip_limits.tdp_limit;
		}

		return;
	}

	LOG_DBG("No valid CCFGOVR bank found; using cmfwcfg as-is");
}

#endif /* CONFIG_BH_FWTABLE_CCFGOVR */

static int tt_bh_fwtable_init(const struct device *dev)
{
	int rc;

	rc = tt_bh_fwtable_load(dev, BH_FWTABLE_BOARDCFG);
	if (rc < 0) {
		if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY)) {
			LOG_WRN("Failed to load %s table, continuing in SMC recovery mode",
				"Board Config");
			/*
			 * Returning 0 here keeps the hardware init status okay,
			 * so pyluwen will interface with the chip
			 */
			return 0;
		}
		return rc;
	}

	rc = tt_bh_fwtable_load(dev, BH_FWTABLE_FLSHINFO);
	if (rc < 0) {
		return rc;
	}

	rc = tt_bh_fwtable_load(dev, BH_FWTABLE_CMFWCFG);
	if (rc < 0) {
		return rc;
	}

#ifdef CONFIG_BH_FWTABLE_CCFGOVR
	tt_bh_fwtable_apply_ccfgovr(dev);
#endif

	return 0;
}

#define DEFINE_BH_FWTABLE(_inst)                                                                   \
	static struct bh_fwtable_data bh_fwtable_data_##_inst;                                     \
	static const struct bh_fwtable_config bh_fwtable_config_##_inst = {                        \
		.flash = DEVICE_DT_GET(DT_INST_PHANDLE(_inst, flash_dev)),                         \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(_inst, tt_bh_fwtable_init, NULL, &bh_fwtable_data_##_inst,           \
			      &bh_fwtable_config_##_inst, POST_KERNEL,                             \
			      CONFIG_BH_FWTABLE_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(DEFINE_BH_FWTABLE)
