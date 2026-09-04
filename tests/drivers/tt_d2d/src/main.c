/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/ztest.h>

#include <soc.h>

#include <zephyr/drivers/misc/tt_d2d.h>

#include "chip_init.h"
#include "d2d_fw_blob.h"

/*
 * Mirrors of the driver's private layout. Duplicated on purpose: the test is
 * checking that the driver puts things at these offsets, so sharing a header
 * would let both drift together without failing.
 */
#define SRAM_OFFSET     0x2000U
#define CPU_CTRL_OFFSET 0x1800U
#define VERSION_OFFSET  0x1014U
#define CFG_OFFSET      0xFCC0U
#define IMAGE_MAX       CFG_OFFSET

#define CFG_MAGIC_VALUE 0x1234BEAFU
#define CPU_CTRL_HALT   0x00010001U
#define CPU_CTRL_START  0x00010000U
#define PROBE_PATTERN   0xD2D0F00DU

#define CFG_IDX_MAGIC            0U
#define CFG_IDX_BITS_PER_CHANNEL 1U
#define CFG_IDX_PLL_FREQ_GHZ     4U
#define CFG_IDX_BYPASS_VREG      7U
#define CFG_IDX_BYPASS_PHY       17U
#define CFG_IDX_DISABLE_SIDEBAND 18U

#define D2D0_NODE DT_NODELABEL(d2d0)
#define D2D1_NODE DT_NODELABEL(d2d1)

static const struct device *const d2d0 = DEVICE_DT_GET(D2D0_NODE);
static const struct device *const d2d1 = DEVICE_DT_GET(D2D1_NODE);

#define D2D0_BASE DT_REG_ADDR(D2D0_NODE)
#define D2D1_BASE DT_REG_ADDR(D2D1_NODE)

/* Word i is distinguishable from both zero and its own offset. */
#define IMAGE_WORDS 64U
static uint32_t test_image[IMAGE_WORDS];

static uint32_t cfg_read(uintptr_t base, uint32_t index)
{
	return sys_read32(base + SRAM_OFFSET + CFG_OFFSET + (index * sizeof(uint32_t)));
}

/*
 * The only thing visible from the host while the tests run are the scratch registers
 * which the harness prints on every poll.
 * Each step publishes which test it is in and how far it got, so a run that stops
 * responding names the call it stopped in instead of just failing to reach the
 * pass token. scratch[4] carries the value the step just produced for example a
 * driver return code.
 *
 * The tag makes step 0 distinguishable from a scratch register nobody has
 * written yet.
 */
#define STEP(test, step) WRITE_SCRATCH(3, 0xB0000000U | ((test) << 8) | (step))
#define STEP_RESULT(val) WRITE_SCRATCH(4, (uint32_t)(val))

#define T_SETUP  0x00U
#define T_IMAGE  0x10U
#define T_CLEAR  0x20U
#define T_CONFIG 0x30U
#define T_BYPASS 0x40U
#define T_ARGS   0x50U
#define T_START  0x60U
#define T_BLOB   0x70U
#define T_EXEC   0x80U

/*
 * A failing assertion ends its test but not the suite, and the suite's pass
 * token is only written if every test passed -- so without this a run with one
 * broken test is indistinguishable from a run with six. Each test sets its bit
 * on the way out; whatever is missing at the end failed.
 */
static uint32_t passed_mask;

#define PASSED(bit)                                                                                \
	do {                                                                                       \
		passed_mask |= BIT(bit);                                                           \
		WRITE_SCRATCH(1, 0xA5000000U | passed_mask);                                       \
	} while (0)

#define P_IMAGE  0
#define P_CLEAR  1
#define P_CONFIG 2
#define P_BYPASS 3
#define P_ARGS   4
#define P_START  5
#define P_BLOB   6
#define P_EXEC   7

static void *tt_d2d_setup(void)
{
	for (uint32_t i = 0; i < IMAGE_WORDS; i++) {
		test_image[i] = 0xD2D00000U + i;
	}

	STEP(T_SETUP, 1);
	zassert_true(device_is_ready(d2d0), "d2d0 not ready");
	zassert_true(device_is_ready(d2d1), "d2d1 not ready");

	/* Without this the tiles do not answer and every load fails -ENODEV. */
	STEP(T_SETUP, 2);
	zassert_ok(tt_d2d_test_chip_init(), "chip init failed");

	STEP(T_SETUP, 3);
	zassert_ok(tt_d2d_reset_release(d2d0), "d2d0 reset release failed");
	STEP(T_SETUP, 4);
	zassert_ok(tt_d2d_reset_release(d2d1), "d2d1 reset release failed");

	/*
	 * Walk d2d0 by hand up to the point tt_d2d_load_fw() starts from, and
	 * leave each answer in its own scratch register rather than the shared
	 * step result, so all three survive to the end of the run. A load that
	 * wedges is then attributable: if these three are right, the tile was
	 * reachable and the driver lost it later.
	 *
	 * d2d_version is read-only and valid with the Rocket still in reset,
	 * which is what the reference bring-up checks first; cpu_ctrl confirms
	 * uncore reset actually cleared; the SRAM word confirms the window that
	 * clearing opens is writable.
	 */
	STEP(T_SETUP, 5);
	WRITE_SCRATCH(5, sys_read32(D2D0_BASE + VERSION_OFFSET));

	STEP(T_SETUP, 6);
	sys_write32(CPU_CTRL_HALT, D2D0_BASE + CPU_CTRL_OFFSET);
	STEP(T_SETUP, 7);
	WRITE_SCRATCH(6, sys_read32(D2D0_BASE + CPU_CTRL_OFFSET));

	STEP(T_SETUP, 8);
	sys_write32(PROBE_PATTERN, D2D0_BASE + SRAM_OFFSET);
	STEP(T_SETUP, 9);
	WRITE_SCRATCH(7, sys_read32(D2D0_BASE + SRAM_OFFSET));

	STEP(T_SETUP, 10);

	return NULL;
}

ZTEST(tt_d2d, test_load_writes_image)
{
	int ret;

	STEP(T_IMAGE, 1);
	ret = tt_d2d_load_fw(d2d0, (const uint8_t *)test_image, sizeof(test_image));
	STEP(T_IMAGE, 2);
	STEP_RESULT(ret);
	zassert_ok(ret);

	STEP(T_IMAGE, 3);
	for (uint32_t i = 0; i < IMAGE_WORDS; i++) {
		uint32_t got = sys_read32(D2D0_BASE + SRAM_OFFSET + (i * sizeof(uint32_t)));

		STEP_RESULT(got);
		zassert_equal(got, test_image[i], "word %u: expected 0x%08x got 0x%08x", i,
			      test_image[i], got);
	}

	PASSED(P_IMAGE);
}

ZTEST(tt_d2d, test_load_clears_sram_beyond_image)
{
	/* Dirty a word past the image so a load that only writes the image
	 * itself, without clearing, would leave it behind.
	 */
	uintptr_t stale = D2D0_BASE + SRAM_OFFSET + sizeof(test_image);
	int ret;

	STEP(T_CLEAR, 1);
	sys_write32(0xDEADBEEF, stale);

	STEP(T_CLEAR, 2);
	ret = tt_d2d_load_fw(d2d0, (const uint8_t *)test_image, sizeof(test_image));
	STEP(T_CLEAR, 3);
	STEP_RESULT(ret);
	zassert_ok(ret);

	STEP(T_CLEAR, 4);
	STEP_RESULT(sys_read32(stale));
	zassert_equal(sys_read32(stale), 0, "SRAM past the image was not cleared");

	PASSED(P_CLEAR);
}

ZTEST(tt_d2d, test_load_writes_config_block)
{
	int ret;

	STEP(T_CONFIG, 1);
	ret = tt_d2d_load_fw(d2d0, (const uint8_t *)test_image, sizeof(test_image));
	STEP(T_CONFIG, 2);
	STEP_RESULT(ret);
	zassert_ok(ret);

	STEP(T_CONFIG, 3);
	STEP_RESULT(cfg_read(D2D0_BASE, CFG_IDX_MAGIC));
	zassert_equal(cfg_read(D2D0_BASE, CFG_IDX_MAGIC), CFG_MAGIC_VALUE,
		      "config magic missing; firmware would ignore the block");

	STEP(T_CONFIG, 4);
	STEP_RESULT(cfg_read(D2D0_BASE, CFG_IDX_BITS_PER_CHANNEL));
	zassert_equal(cfg_read(D2D0_BASE, CFG_IDX_BITS_PER_CHANNEL),
		      DT_PROP(D2D0_NODE, bits_per_channel));

	STEP(T_CONFIG, 5);
	STEP_RESULT(cfg_read(D2D0_BASE, CFG_IDX_PLL_FREQ_GHZ));
	zassert_equal(cfg_read(D2D0_BASE, CFG_IDX_PLL_FREQ_GHZ), DT_PROP(D2D0_NODE, pll_freq_ghz));

	PASSED(P_CONFIG);
}

ZTEST(tt_d2d, test_bypass_flags_follow_devicetree)
{
	int ret;

	STEP(T_BYPASS, 1);
	ret = tt_d2d_load_fw(d2d0, (const uint8_t *)test_image, sizeof(test_image));
	STEP(T_BYPASS, 2);
	STEP_RESULT(ret);
	zassert_ok(ret);

	STEP(T_BYPASS, 3);
	ret = tt_d2d_load_fw(d2d1, (const uint8_t *)test_image, sizeof(test_image));
	STEP(T_BYPASS, 4);
	STEP_RESULT(ret);
	zassert_ok(ret);

	/* d2d0 sets all three bypasses in the overlay, d2d1 sets none. */
	STEP(T_BYPASS, 5);
	STEP_RESULT(cfg_read(D2D0_BASE, CFG_IDX_BYPASS_PHY));
	zassert_equal(cfg_read(D2D0_BASE, CFG_IDX_BYPASS_PHY), 1);
	zassert_equal(cfg_read(D2D0_BASE, CFG_IDX_BYPASS_VREG), 1);
	zassert_equal(cfg_read(D2D0_BASE, CFG_IDX_DISABLE_SIDEBAND), 1);

	STEP(T_BYPASS, 6);
	STEP_RESULT(cfg_read(D2D1_BASE, CFG_IDX_BYPASS_PHY));
	zassert_equal(cfg_read(D2D1_BASE, CFG_IDX_BYPASS_PHY), 0);
	zassert_equal(cfg_read(D2D1_BASE, CFG_IDX_BYPASS_VREG), 0);
	zassert_equal(cfg_read(D2D1_BASE, CFG_IDX_DISABLE_SIDEBAND), 0);

	PASSED(P_BYPASS);
}

ZTEST(tt_d2d, test_load_rejects_bad_arguments)
{
	STEP(T_ARGS, 1);
	zassert_equal(tt_d2d_load_fw(d2d0, NULL, sizeof(test_image)), -EINVAL, "NULL image");
	zassert_equal(tt_d2d_load_fw(d2d0, (const uint8_t *)test_image, 0), -EINVAL, "empty image");
	zassert_equal(tt_d2d_load_fw(d2d0, (const uint8_t *)test_image, 6), -EINVAL,
		      "image length not a multiple of 4");

	/* Size is validated before the buffer is read, so a short buffer with an
	 * oversized length is safe and avoids allocating 64 KB in the test.
	 */
	zassert_equal(tt_d2d_load_fw(d2d0, (const uint8_t *)test_image, IMAGE_MAX + 4), -ENOSPC,
		      "image overlapping the config block");

	PASSED(P_ARGS);
}

ZTEST(tt_d2d, test_load_halts_and_start_releases)
{
	int ret;

	STEP(T_START, 1);
	ret = tt_d2d_load_fw(d2d0, (const uint8_t *)test_image, sizeof(test_image));
	STEP(T_START, 2);
	STEP_RESULT(ret);
	zassert_ok(ret);

	STEP(T_START, 3);
	STEP_RESULT(sys_read32(D2D0_BASE + CPU_CTRL_OFFSET));
	zassert_equal(sys_read32(D2D0_BASE + CPU_CTRL_OFFSET), CPU_CTRL_HALT,
		      "Rocket should still be in reset after a load");

	STEP(T_START, 4);
	zassert_ok(tt_d2d_start(d2d0));
	STEP(T_START, 5);
	STEP_RESULT(sys_read32(D2D0_BASE + CPU_CTRL_OFFSET));
	/* Only that core reset was deasserted. Whether the Rocket then fetched
	 * anything is test_started_firmware_executes' business; this readback
	 * looks the same either way.
	 */
	zassert_equal(sys_read32(D2D0_BASE + CPU_CTRL_OFFSET), CPU_CTRL_START,
		      "start did not clear core reset");

	PASSED(P_START);
}

/*
 * The firmware image is not compiled into the driver: it is a separate file,
 * embedded by this test and handed to the loader as a plain buffer. This is
 * the whole of that path, from the generated initialisers through to SRAM, so
 * a drop that lands truncated fails here rather than as a link that will not
 * train much later.
 *
 * The image under test is this test's generated stand-in, not the real
 * firmware; what is being checked is the mechanism carrying it.
 */
ZTEST(tt_d2d, test_linked_fw_image_loads)
{
	const uint8_t *img;
	size_t size = 0;
	int ret;

	STEP(T_BLOB, 1);
	img = d2d_fw_image(&size);
	STEP_RESULT(size);
	zassert_not_null(img, "no image linked in");
	zassert_true(size > 0, "linked image is empty");
	zassert_equal(size % sizeof(uint32_t), 0, "image of %zu bytes is not word-sized", size);
	zassert_true(size <= IMAGE_MAX, "image of %zu bytes overlaps the config block", size);

#ifdef D2D_FW_STUB_UNIT
	/* Only the generated stub has contents this test can predict, and
	 * checking them confirms the bytes are the file's rather than zeroes
	 * from a section that was allocated but never filled.
	 */
	{
		static const char unit[] = D2D_FW_STUB_UNIT;
		const size_t unit_len = sizeof(unit) - 1;

		zassert_equal(size, unit_len * D2D_FW_STUB_REPEAT, "stub is %zu bytes", size);

		STEP(T_BLOB, 2);
		for (size_t i = 0; i < size; i++) {
			STEP_RESULT(img[i]);
			zassert_equal(img[i], unit[i % unit_len],
				      "image byte %zu is 0x%02x, expected 0x%02x", i, img[i],
				      unit[i % unit_len]);
		}
	}
#endif

	STEP(T_BLOB, 3);
	ret = tt_d2d_load_fw(d2d0, img, size);
	STEP(T_BLOB, 4);
	STEP_RESULT(ret);
	zassert_ok(ret, "loading the linked image failed");

	STEP(T_BLOB, 5);
	for (size_t off = 0; off < size; off += sizeof(uint32_t)) {
		uint32_t got = sys_read32(D2D0_BASE + SRAM_OFFSET + off);
		uint32_t want = sys_get_le32(&img[off]);

		STEP_RESULT(got);
		zassert_equal(got, want, "SRAM +0x%zx is 0x%08x, expected 0x%08x", off, got, want);
	}

	PASSED(P_BLOB);
}

/*
 * Everything above this point stops at the loader: it shows bytes arriving in
 * SRAM and the control register accepting a write, neither of which needs the
 * Rocket to have executed a single instruction.
 *
 * This is the one test that distinguishes a core that ran from a core that is
 * wedged, and it does it without a link partner, a PHY, or anything the
 * emulation bypasses turn off. The loader zeroes the whole window before
 * copying, so between the end of the image and the config block there is a
 * region that is known to be zero and that the loader will not touch again.
 * Anything non-zero there after the Rocket is released was put there by the
 * Rocket. Confirming the region really is zero first is what makes that a
 * proof rather than an inference.
 *
 * A stack push is the earliest such write and lands near the top of the
 * window, hence the downward scan: when the firmware is alive the search ends
 * almost immediately, and only a genuine failure pays for the full sweep.
 */
/*
 * A live Rocket has written something before the first scan finishes: on
 * emulation the sweep alone takes well over a minute, which dwarfs the time
 * the core needs to reach its first store. The retries are there for margin,
 * not because a pass is expected to need them, and the count is kept low
 * deliberately -- each one is another full sweep, and enough of them would
 * turn a clean failure into a harness timeout with nothing to read.
 */
#define EXEC_ATTEMPTS     3U
#define EXEC_SETTLE_ITERS 0x1000U

/* Offset of the highest non-zero word below `end`, or `end` if there is none. */
static size_t highest_nonzero(uintptr_t sram, size_t start, size_t end)
{
	for (size_t off = end; off > start;) {
		off -= sizeof(uint32_t);

		if (sys_read32(sram + off) != 0) {
			return off;
		}
	}

	return end;
}

ZTEST(tt_d2d, test_started_firmware_executes)
{
	const uintptr_t sram = D2D0_BASE + SRAM_OFFSET;
	const uint8_t *img;
	size_t size = 0;
	size_t hit;
	int ret;

#ifdef D2D_FW_STUB_UNIT
	/* The generated stand-in is ASCII, not instructions. Releasing a core
	 * onto it would prove nothing and leave it in an arbitrary state.
	 */
	ztest_test_skip();
#endif

	STEP(T_EXEC, 1);
	img = d2d_fw_image(&size);
	zassert_true(size > 0 && size < IMAGE_MAX, "no room between image and config block");

	STEP(T_EXEC, 2);
	ret = tt_d2d_load_fw(d2d0, img, size);
	STEP_RESULT(ret);
	zassert_ok(ret);

	STEP(T_EXEC, 3);
	hit = highest_nonzero(sram, size, IMAGE_MAX);
	STEP_RESULT(hit);
	zassert_equal(hit, IMAGE_MAX,
		      "SRAM +0x%zx is already non-zero with the Rocket still in "
		      "reset, so a later write there would prove nothing",
		      hit);

	STEP(T_EXEC, 4);
	zassert_ok(tt_d2d_start(d2d0));

	for (uint32_t attempt = 0; attempt < EXEC_ATTEMPTS; attempt++) {
		STEP(T_EXEC, 5);
		for (volatile uint32_t i = 0; i < EXEC_SETTLE_ITERS; i++) {
		}

		STEP(T_EXEC, 6);
		hit = highest_nonzero(sram, size, IMAGE_MAX);
		if (hit != IMAGE_MAX) {
			break;
		}
	}

	STEP_RESULT(hit);
	zassert_not_equal(hit, IMAGE_MAX,
			  "SRAM is untouched after releasing the Rocket: it is not executing the "
			  "image. Check that the load landed (earlier tests) and that the core "
			  "reset in cpu_ctrl actually deasserted");

	PASSED(P_EXEC);
}

/*
 * The harness's verdict is ztest's own, and ztest can fail a run for reasons
 * no individual test reports: a result recorded outside a test body, or
 * per-test stats that do not add up. With no readable console, publishing the
 * framework's counters is what separates "a test failed" from "every test
 * passed and the run was failed anyway".
 */
static void tt_d2d_teardown(void *fixture)
{
	uint32_t run = 0;
	uint32_t pass = 0;
	uint32_t fail = 0;
	uint32_t skip = 0;
	uint32_t inconsistent = 0;

	ARG_UNUSED(fixture);

	for (struct ztest_unit_test *t = _ztest_unit_test_list_start; t < _ztest_unit_test_list_end;
	     t++) {
		run += t->stats->run_count;
		pass += t->stats->pass_count;
		fail += t->stats->fail_count;
		skip += t->stats->skip_count;

		if (t->stats->pass_count + t->stats->fail_count + t->stats->skip_count !=
		    t->stats->run_count) {
			inconsistent = 1;
		}
	}

	WRITE_SCRATCH(4, 0xC0000000U | (run << 16) | (pass << 12) | (fail << 8) | (skip << 4) |
				 inconsistent);
}

ZTEST_SUITE(tt_d2d, NULL, tt_d2d_setup, NULL, NULL, tt_d2d_teardown);
