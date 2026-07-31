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

/* The i3c-target-dev alias names the instance the app serves as an I3C target. */
#if DT_HAS_ALIAS(i3c_target_dev)
#include <zephyr/drivers/i3c.h>
#include <zephyr/drivers/i3c/target_device.h>
#endif

LOG_MODULE_REGISTER(dm_test_app, LOG_LEVEL_INF);

#if DT_HAS_ALIAS(i3c_target_dev)
BUILD_ASSERT(IS_ENABLED(CONFIG_I3C_TARGET), "i3c-target-dev needs CONFIG_I3C_TARGET");
BUILD_ASSERT(DT_PROP_OR(DT_ALIAS(i3c_target_dev), target_mode, 0),
	     "i3c-target-dev must point at an instance with target-mode");

static const struct device *i3c_dev = DEVICE_DT_GET(DT_ALIAS(i3c_target_dev));

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
	SPI_DT_SPEC_GET(DT_NODELABEL(max7221), SPI_WORD_SET(8) | SPI_OP_MODE_MASTER);

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

/*
 * 180-degree rotation, for a fixture that mounts the board upside down.
 *
 * Two independent transforms, and both are needed -- doing either alone leaves
 * the display unreadable:
 *
 *   X: the digit order reverses. What was the leftmost physical position is now
 *      the viewer's rightmost.
 *   Y: each glyph is rotated, which on seven segments is a segment swap:
 *      a<->d, b<->e, c<->f, with g mapping to itself. The decimal point has no
 *      rotated counterpart and is left where it is.
 *
 * Rotating is not the same as re-reading a mirrored display: some glyphs are
 * symmetric (0 2 5 8 and blank), but others become a *different* character --
 * 6 and 9 swap, and 7 becomes L. So the transform has to be applied to the
 * segment data; you cannot compensate by squinting.
 *
 * The transform is its own inverse, so `post flip` toggles cleanly.
 *
 * Defaults to enabled to match the current fixture. `post flip off` restores
 * the upright mapping without a reflash, since fixtures change more often than
 * firmware.
 */
static bool post_flipped = true;

static uint8_t post_rotate_segments(uint8_t segs)
{
	uint8_t out = segs & (SEG_G | SEG_DP);

	if (segs & SEG_A) {
		out |= SEG_D;
	}
	if (segs & SEG_D) {
		out |= SEG_A;
	}
	if (segs & SEG_B) {
		out |= SEG_E;
	}
	if (segs & SEG_E) {
		out |= SEG_B;
	}
	if (segs & SEG_C) {
		out |= SEG_F;
	}
	if (segs & SEG_F) {
		out |= SEG_C;
	}
	return out;
}

/* The MAX7221 digit that shows reading-order position `pos`. */
static uint8_t post_dig_for(int pos)
{
	return post_pos_to_dig[post_flipped ? (8 - 1 - pos) : pos];
}

/* Write segment patterns given in reading order (leftmost first). */
static int post_display_positions(const uint8_t pos_segs[8])
{
	uint8_t segs[8] = {0};

	for (int pos = 0; pos < 8; pos++) {
		uint8_t value = pos_segs[pos];

		if (post_flipped) {
			value = post_rotate_segments(value);
		}
		segs[post_dig_for(pos)] = value;
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
		shell_print(sh, "  position %d from left lit (DIG%d)", pos + 1, post_dig_for(pos));
		k_sleep(K_MSEC(1200));
	}
	return 0;
}

/*
 * Full seven-segment character library: '0'-'9', 'A'-'Z', space and a little
 * punctuation.
 *
 * Seven segments cannot draw the whole alphabet, so this table is explicit about
 * how each letter is compromised rather than quietly showing something wrong:
 *
 *   - POST_ALPHA_LOWERCASE (B D N R T): the uppercase glyph would be
 *     indistinguishable from a digit -- uppercase 'B' reads as '8', 'D' as '0' --
 *     so the lowercase form is used. Legible, just not the case you typed.
 *   - POST_ALPHA_APPROX (K M V W X): no seven-segment form exists. K and X fall
 *     back to the 'H' shape, M to a top-and-shoulders shape, V and W to the 'U'
 *     shape. These are stand-ins and will not read as the intended letter.
 *   - Inherently ambiguous with digits, and left that way because the shapes are
 *     genuinely identical: I/1, O/0, S/5, Z/2, and G is close to 6.
 *
 * '$' has no form either and renders as 'S', which is the conventional stand-in.
 */
#define POST_GLYPH_NONE 0xFF

/* 'A' through 'Z'. */
static const uint8_t post_font_alpha[26] = {
	SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G, /* A */
	SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,         /* b (lowercase) */
	SEG_A | SEG_D | SEG_E | SEG_F,                 /* C */
	SEG_B | SEG_C | SEG_D | SEG_E | SEG_G,         /* d (lowercase) */
	SEG_A | SEG_D | SEG_E | SEG_F | SEG_G,         /* E */
	SEG_A | SEG_E | SEG_F | SEG_G,                 /* F */
	SEG_A | SEG_C | SEG_D | SEG_E | SEG_F,         /* G (close to 6) */
	SEG_B | SEG_C | SEG_E | SEG_F | SEG_G,         /* H */
	SEG_B | SEG_C,                                 /* I (same as 1) */
	SEG_B | SEG_C | SEG_D | SEG_E,                 /* J */
	SEG_B | SEG_C | SEG_E | SEG_F | SEG_G,         /* K -- approx, H shape */
	SEG_D | SEG_E | SEG_F,                         /* L */
	SEG_A | SEG_C | SEG_E,                         /* M -- approx */
	SEG_C | SEG_E | SEG_G,                         /* n (lowercase) */
	SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F, /* O (same as 0) */
	SEG_A | SEG_B | SEG_E | SEG_F | SEG_G,         /* P */
	SEG_A | SEG_B | SEG_C | SEG_F | SEG_G,         /* Q */
	SEG_E | SEG_G,                                 /* r (lowercase) */
	SEG_A | SEG_C | SEG_D | SEG_F | SEG_G,         /* S (same as 5) */
	SEG_D | SEG_E | SEG_F | SEG_G,                 /* t (lowercase) */
	SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,         /* U */
	SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,         /* V -- approx, U shape */
	SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,         /* W -- approx, U shape */
	SEG_B | SEG_C | SEG_E | SEG_F | SEG_G,         /* X -- approx, H shape */
	SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,         /* Y */
	SEG_A | SEG_B | SEG_D | SEG_E | SEG_G,         /* Z (same as 2) */
};

/* Bit n = letter 'A' + n. Rendered lowercase because uppercase would read as a digit. */
#define POST_ALPHA_LOWERCASE                                                                       \
	(BIT('B' - 'A') | BIT('D' - 'A') | BIT('N' - 'A') | BIT('R' - 'A') | BIT('T' - 'A'))

/* Bit n = letter 'A' + n. No seven-segment form; the glyph is a stand-in. */
#define POST_ALPHA_APPROX                                                                          \
	(BIT('K' - 'A') | BIT('M' - 'A') | BIT('V' - 'A') | BIT('W' - 'A') | BIT('X' - 'A'))

/* True if this letter's glyph is a compromise worth telling the operator about. */
bool post_glyph_is_exact(char ch)
{
	if (ch >= 'a' && ch <= 'z') {
		ch = ch - 'a' + 'A';
	}
	if (ch < 'A' || ch > 'Z') {
		return ch != '$';
	}
	return !(BIT(ch - 'A') & (POST_ALPHA_LOWERCASE | POST_ALPHA_APPROX));
}

static uint8_t post_glyph(char ch)
{
	if (ch >= '0' && ch <= '9') {
		return post_font_hex[ch - '0'];
	}
	if (ch >= 'a' && ch <= 'z') {
		ch = ch - 'a' + 'A';
	}
	if (ch >= 'A' && ch <= 'Z') {
		return post_font_alpha[ch - 'A'];
	}

	switch (ch) {
	case ' ':
		return 0;
	case '$':
		return post_font_alpha['S' - 'A']; /* no glyph; 'S' stands in */
	case '-':
		return SEG_G;
	case '_':
		return SEG_D;
	case '=':
		return SEG_D | SEG_G;
	case '.':
		return SEG_DP;
	case '?':
		return SEG_A | SEG_B | SEG_E | SEG_G;
	case '*':
		return SEG_A | SEG_B | SEG_F | SEG_G; /* degree-ish */
	default:
		return POST_GLYPH_NONE;
	}
}

/* Show an arbitrary string, left-aligned, blanking any unused digits. */
static int post_text_show(const struct shell *sh, const char *text)
{
	uint8_t pos_segs[8] = {0};
	char compromised[9] = {0};
	size_t n_compromised = 0;
	size_t len = strlen(text);

	if (len > ARRAY_SIZE(pos_segs)) {
		shell_error(sh, "\"%s\" is %u characters; the display has %u digits", text,
			    (unsigned int)len, (unsigned int)ARRAY_SIZE(pos_segs));
		return -EINVAL;
	}

	for (size_t i = 0; i < len; i++) {
		uint8_t segs = post_glyph(text[i]);

		if (segs == POST_GLYPH_NONE) {
			shell_error(sh, "no seven-segment glyph for '%c'", text[i]);
			return -EINVAL;
		}
		pos_segs[i] = segs;

		if (!post_glyph_is_exact(text[i])) {
			compromised[n_compromised++] = text[i];
		}
	}

	if (n_compromised > 0) {
		shell_warn(sh, "%s rendered as a lowercase or approximate glyph", compromised);
	}

	LOG_INF("POST text \"%s\"", text);
	return post_display_positions(pos_segs);
}

/*
 * Scroll a message right-to-left across the 8 digits.
 *
 * The segment buffer is padded with a blank display's worth on each side, so the
 * text slides in from the right and out to the left rather than appearing
 * mid-screen. Each frame is one position further along that buffer. The scroll
 * ends resting on the message's first characters, because finishing on a blank
 * display looks like a failure.
 *
 * This blocks the shell for repeats * (len + 8) * POST_BANNER_STEP_MS, the same
 * way `post walk` does.
 */
#define POST_BANNER_MAX         32
#define POST_BANNER_STEP_MS     220
#define POST_BANNER_MAX_REPEATS 20

static int post_banner_show(const struct shell *sh, const char *text, unsigned int repeats)
{
	uint8_t segs[POST_BANNER_MAX + 2 * 8] = {0};
	size_t len = strlen(text);

	if (len == 0 || len > POST_BANNER_MAX) {
		shell_error(sh, "banner text must be 1 to %u characters",
			    (unsigned int)POST_BANNER_MAX);
		return -EINVAL;
	}
	if (repeats == 0 || repeats > POST_BANNER_MAX_REPEATS) {
		shell_error(sh, "repeats must be 1 to %u", POST_BANNER_MAX_REPEATS);
		return -EINVAL;
	}

	for (size_t i = 0; i < len; i++) {
		uint8_t glyph = post_glyph(text[i]);

		if (glyph == POST_GLYPH_NONE) {
			shell_error(sh, "no seven-segment glyph for '%c'", text[i]);
			return -EINVAL;
		}
		segs[8 + i] = glyph;
	}

	LOG_INF("POST banner \"%s\" x%u", text, repeats);

	for (unsigned int pass = 0; pass < repeats; pass++) {
		/* Frame 0 is an all-blank window, so start at 1. */
		for (size_t frame = 1; frame <= len + 8; frame++) {
			int ret = post_display_positions(&segs[frame]);

			if (ret < 0) {
				return ret;
			}
			k_sleep(K_MSEC(POST_BANNER_STEP_MS));
		}
	}

	/* Rest on the start of the message rather than on a blank display. */
	return post_display_positions(&segs[8]);
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
	} else if (strcmp(argv[1], "flip") == 0) {
		if (argc > 2) {
			if (strcmp(argv[2], "on") == 0) {
				post_flipped = true;
			} else if (strcmp(argv[2], "off") == 0) {
				post_flipped = false;
			} else {
				shell_error(sh, "usage: post flip [on|off]");
				return -EINVAL;
			}
		} else {
			post_flipped = !post_flipped;
		}
		ret = post_code_show(0x0001);
		shell_print(sh, "display rotation %s",
			    post_flipped ? "ON (fixture upside down)" : "OFF (upright)");
	} else if (strcmp(argv[1], "banner") == 0) {
		unsigned int repeats = 1;

		if (argc < 3) {
			shell_error(sh, "usage: post banner <string> [repeats]");
			return -EINVAL;
		}
		if (argc > 3) {
			repeats = (unsigned int)strtoul(argv[3], NULL, 10);
		}
		ret = post_banner_show(sh, argv[2], repeats);
		if (ret == 0) {
			shell_print(sh, "scrolled \"%s\" %u time(s)", argv[2], repeats);
		}
	} else if (strcmp(argv[1], "text") == 0) {
		if (argc < 3) {
			shell_error(sh, "usage: post text <string>");
			return -EINVAL;
		}
		ret = post_text_show(sh, argv[2]);
		if (ret == 0) {
			shell_print(sh, "displayed \"%s\"", argv[2]);
		}
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

SHELL_CMD_ARG_REGISTER(
	post, NULL,
	"MAX7221 POST-code display\n"
	"Usage: post <hex code>   e.g. post 0042\n"
	"       post test         all segments on, every digit\n"
	"       post walk         light one digit at a time\n"
	"       post map          write numeral N into digit N\n"
	"       post banner <str> [n]  scroll up to 32 characters, n times\n"
	"       post flip [on|off]     180-degree rotation for a flipped\n"
	"                              fixture; toggles when given no argument\n"
	"       post text <str>   show up to 8 characters, e.g. post text DANIEL\n"
	"                         A-Z, 0-9, space and - _ = . ? * are renderable\n"
	"                         B D N R T show lowercase; K M V W X are approximations",
	cmd_post, 2, 2);
#endif /* max7221 */

int main(void)
{
	LOG_INF("Hello World! This is dm_test_app running on %s", CONFIG_BOARD);

#if DT_NODE_EXISTS(DT_NODELABEL(max7221))
	/* Boot POST code: 0x0001 = firmware reached main(). */
	post_code_show(0x0001);
#endif

#if DT_HAS_ALIAS(i3c_target_dev)
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
