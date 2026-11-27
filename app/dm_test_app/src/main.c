/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if DT_NODE_EXISTS(DT_NODELABEL(max7221))
#include <zephyr/drivers/spi.h>
#endif

#ifdef CONFIG_I3C_TARGET
#include <zephyr/drivers/i3c.h>
#include <zephyr/drivers/i3c/target_device.h>
#endif

LOG_MODULE_REGISTER(dm_test_app, LOG_LEVEL_INF);

#ifdef CONFIG_I3C_TARGET
static const struct device *i3c_dev = DEVICE_DT_GET(DT_NODELABEL(i3c1));

static uint8_t value;

static int target_prefill(void)
{
	return i3c_target_tx_write(i3c_dev, NULL, 8U, I3C_MSG_HDR_MODE0);
}

/* I3C target callback functions */
static int i3c_target_write_requested_cb(struct i3c_target_config *config)
{
	ARG_UNUSED(config);
	LOG_INF("I3C Target: Write requested callback entered");
	return 0;
}

static int i3c_target_write_received_cb(struct i3c_target_config *config, uint8_t val)
{
	ARG_UNUSED(config);
	LOG_INF("I3C Target: Write received callback entered, received: 0x%02x", val);
	return 0;
}

static int i3c_target_read_requested_cb(struct i3c_target_config *config, uint8_t *val)
{
	ARG_UNUSED(config);
	ARG_UNUSED(val);
	target_prefill();
	return 0;
}

static int i3c_target_read_processed_cb(struct i3c_target_config *config, uint8_t *val)
{
	ARG_UNUSED(config);
	*val = value++; /* Return dummy data */
	return 0;
}

static int i3c_target_stop_cb(struct i3c_target_config *config)
{
	ARG_UNUSED(config);
	LOG_INF("I3C Target: Stop callback entered");
	return 0;
}

/* I3C target callbacks structure */
static const struct i3c_target_callbacks i3c_target_callbacks = {
	.write_requested_cb = i3c_target_write_requested_cb,
	.write_received_cb = i3c_target_write_received_cb,
	.read_requested_cb = i3c_target_read_requested_cb,
	.read_processed_cb = i3c_target_read_processed_cb,
	.stop_cb = i3c_target_stop_cb,
};

/* I3C target configuration */
static struct i3c_target_config i3c_target_config = {
	.callbacks = &i3c_target_callbacks,
};

#endif

#if DT_NODE_EXISTS(DT_NODELABEL(max7221))
/*
 * POST-code display on the MAX7221 (U67) driving two LDQ-N514RI 4-digit
 * displays -- 8 digits total.
 *
 * The Zephyr max7219 display driver initialises the part (out of shutdown,
 * no-decode, intensity, scan limit); this writes the digit registers directly
 * over the same SPI spec, which keeps CS and clock timing under the SPI
 * driver's control. Frames assembled from separate shell commands do not latch
 * reliably, so POST codes are written here rather than from the host.
 */
#define MAX7221_REG_DIGIT0 0x01

/* No-decode segment mapping (datasheet Table 6): D7=DP, D6..D0 = A,B,C,D,E,F,G */
#define SEG_A  0x40
#define SEG_B  0x20
#define SEG_C  0x10
#define SEG_D  0x08
#define SEG_E  0x04
#define SEG_F  0x02
#define SEG_G  0x01
#define SEG_DP 0x80

/* Hex font 0-F. */
static const uint8_t post_font_hex[16] = {
	SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,         /* 0 */
	SEG_B | SEG_C,                                         /* 1 */
	SEG_A | SEG_B | SEG_D | SEG_E | SEG_G,                 /* 2 */
	SEG_A | SEG_B | SEG_C | SEG_D | SEG_G,                 /* 3 */
	SEG_B | SEG_C | SEG_F | SEG_G,                         /* 4 */
	SEG_A | SEG_C | SEG_D | SEG_F | SEG_G,                 /* 5 */
	SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,         /* 6 */
	SEG_A | SEG_B | SEG_C,                                 /* 7 */
	SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G, /* 8 */
	SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,         /* 9 */
	SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G,         /* A */
	SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,                 /* b */
	SEG_A | SEG_D | SEG_E | SEG_F,                         /* C */
	SEG_B | SEG_C | SEG_D | SEG_E | SEG_G,                 /* d */
	SEG_A | SEG_D | SEG_E | SEG_F | SEG_G,                 /* E */
	SEG_A | SEG_E | SEG_F | SEG_G,                         /* F */
};

/* Letters used by the "POST" banner. */
#define GLYPH_P (SEG_A | SEG_B | SEG_E | SEG_F | SEG_G)
#define GLYPH_O (SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F)
#define GLYPH_S (SEG_A | SEG_C | SEG_D | SEG_F | SEG_G)
#define GLYPH_T (SEG_D | SEG_E | SEG_F | SEG_G)

static const struct spi_dt_spec max7221_spi =
	SPI_DT_SPEC_GET(DT_NODELABEL(max7221), SPI_WORD_SET(8) | SPI_OP_MODE_CONTROLLER);

static int max7221_write(uint8_t reg, uint8_t val)
{
	uint8_t frame[2] = {reg, val};
	const struct spi_buf buf = {.buf = frame, .len = sizeof(frame)};
	const struct spi_buf_set tx = {.buffers = &buf, .count = 1};

	return spi_write_dt(&max7221_spi, &tx);
}

/* Write the 8 digit registers from a segment-pattern array, DIG0 first. */
static int post_display_raw(const uint8_t segs[8])
{
	for (int digit = 0; digit < 8; digit++) {
		int ret = max7221_write(MAX7221_REG_DIGIT0 + digit, segs[digit]);

		if (ret < 0) {
			LOG_ERR("MAX7221 digit %d write failed: %d", digit, ret);
			return ret;
		}
	}
	return 0;
}

/*
 * Physical left-to-right position -> MAX7221 DIG index.
 *
 * Measured on the bench with `post map`: the leftmost four positions are D21
 * carrying DIG4..DIG7, then D22 carrying DIG0..DIG3. Everything above this
 * function works in reading order and lets this table do the translation.
 */
static const uint8_t post_pos_to_dig[8] = {4, 5, 6, 7, 0, 1, 2, 3};

/* Write segment patterns given in reading order (leftmost first). */
static int post_display_positions(const uint8_t pos_segs[8])
{
	uint8_t segs[8] = {0};

	for (int pos = 0; pos < 8; pos++) {
		segs[post_pos_to_dig[pos]] = pos_segs[pos];
	}
	return post_display_raw(segs);
}

/* Show "POST" then a 4-digit hex code, in reading order. */
static int post_code_show(uint16_t code)
{
	const uint8_t pos_segs[8] = {
		GLYPH_P,
		GLYPH_O,
		GLYPH_S,
		GLYPH_T,
		post_font_hex[(code >> 12) & 0xF],
		post_font_hex[(code >> 8) & 0xF],
		post_font_hex[(code >> 4) & 0xF],
		post_font_hex[code & 0xF],
	};

	LOG_INF("POST code %04x", code);
	return post_display_positions(pos_segs);
}

/* Light every segment of every digit from the digit registers (not display
 * test) -- proves each digit position works with real data.
 */
static int post_all_segments(void)
{
	uint8_t segs[8];

	memset(segs, 0xFF, sizeof(segs));
	return post_display_raw(segs);
}

/* Write numeral N into digit N: the display then spells out its own physical
 * digit order, so the DIG-to-position mapping can be read at a glance.
 */
static int post_map_digits(void)
{
	uint8_t pos_segs[8];

	/* Numeral N at physical position N: reads 01234567 left to right when
	 * post_pos_to_dig is correct -- a one-glance self-check of the mapping.
	 */
	for (int pos = 0; pos < 8; pos++) {
		pos_segs[pos] = post_font_hex[pos];
	}
	return post_display_positions(pos_segs);
}

/* Walk one digit at a time so a dead position is obvious. */
static int post_walk_digits(const struct shell *sh)
{
	for (int pos = 0; pos < 8; pos++) {
		uint8_t pos_segs[8] = {0};

		pos_segs[pos] = 0xFF;
		int ret = post_display_positions(pos_segs);

		if (ret < 0) {
			return ret;
		}
		shell_print(sh, "  position %d from left lit (DIG%d)", pos + 1,
			    post_pos_to_dig[pos]);
		k_sleep(K_MSEC(1200));
	}
	return 0;
}

static int cmd_post(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (strcmp(argv[1], "test") == 0) {
		ret = post_all_segments();
		if (ret == 0) {
			shell_print(sh, "all digits: every segment on (from digit data)");
		}
	} else if (strcmp(argv[1], "walk") == 0) {
		ret = post_walk_digits(sh);
	} else if (strcmp(argv[1], "map") == 0) {
		ret = post_map_digits();
		if (ret == 0) {
			shell_print(sh, "digit N shows numeral N -- read the display "
					"left to right to get the physical order");
		}
	} else {
		uint16_t code = (uint16_t)strtoul(argv[1], NULL, 16);

		ret = post_code_show(code);
		if (ret == 0) {
			shell_print(sh, "POST %04x displayed", code);
		}
	}

	if (ret < 0) {
		shell_error(sh, "failed to write display: %d", ret);
	}
	return ret;
}

SHELL_CMD_ARG_REGISTER(post, NULL,
		       "MAX7221 POST-code display\n"
		       "Usage: post <hex code>   e.g. post 0042\n"
		       "       post test         all segments on, every digit\n"
		       "       post walk         light one digit at a time\n"
		       "       post map          write numeral N into digit N",
		       cmd_post, 2, 0);
#endif /* max7221 */

int main(void)
{
	LOG_INF("Hello World! This is dm_test_app running on %s", CONFIG_BOARD);

#if DT_NODE_EXISTS(DT_NODELABEL(max7221))
	/* Boot POST code: 0x0001 = firmware reached main(). */
	post_code_show(0x0001);
#endif

#ifdef CONFIG_I3C_TARGET
	int ret;

	if (!device_is_ready(i3c_dev)) {
		LOG_ERR("I3C device is not ready");
		return -ENODEV;
	}

	/* Register I3C target with address 0 */
	ret = i3c_target_register(i3c_dev, &i3c_target_config);
	if (ret < 0) {
		LOG_ERR("Failed to register I3C target: %d", ret);
		return ret;
	}

	target_prefill();

	LOG_INF("I3C target registered successfully");
#endif

	while (1) {
		k_sleep(K_SECONDS(5));
	}

	return 0;
}
