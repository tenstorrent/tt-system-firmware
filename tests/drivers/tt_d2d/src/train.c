/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * D2D link training between two dies.
 *
 * The loader suite stops where the Rocket starts running. Everything it
 * checks happens inside one tile, so it runs on a single die and says nothing
 * about whether a link works. Training is the first step that cannot be faked
 * that way: the two firmwares negotiate with each other over the wires, and
 * neither finishes unless the other is also loaded and running. That makes it
 * the cheapest end-to-end evidence that D2D is alive.
 *
 * In an MM package one link runs, between Mimir0's D2D1 and Mimir1's D2D0.
 * Those two tiles face each other; the remaining tile on each die faces a
 * different neighbour and has no partner in this package, so bringing it up
 * here would only produce a timeout.
 *
 * An MMK package adds Keraunos, and with it a second link: Mimir1's D2D1 to
 * Keraunos0's D2D2. Mimir1 is the die in the middle and trains both of its
 * tiles, which is why a die owns a list of tiles here rather than one. The two
 * links come up in the same pass because the harness releases every Rocket
 * together and will not call the run a pass until all three dies report one --
 * there is no configuration that runs the Keraunos link on its own.
 *
 * Every Mimir boots the same image and every Keraunos the same one, each die
 * picking its end from the chip id the harness leaves in scratch[15].
 *
 * Neither side starts its own Rocket. Both ends have to be released close
 * together or training never converges, and a die can only see itself, so the
 * release is the host's job: each side loads its tile, publishes
 * TS_D2D_WAIT_TRAINING, and waits. Once every die has said that, the host
 * releases all the Rockets in one pass and the two firmwares find each other.
 * That is the drop's own protocol, and following it is what lets the stock
 * two-Mimir harness run this image unmodified.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/ztest.h>

#include <soc.h>

#include <zephyr/drivers/misc/tt_d2d.h>

#include <platform.h>
#include <test_codes.h>

#include "chip_init.h"
#include "d2d_fw_blob.h"

/*
 * Which die this is. The value is a chip id that will eventually come from
 * efuse; until then the harness writes it into the low byte of scratch[15]
 * before releasing the SMC, exactly as the drop's own M2M application reads
 * it.
 */
#define CHIP_ID_MASK   0xFFU
#define CHIP_ID_MIMIR0 0x03U
#define CHIP_ID_MIMIR1 0x13U
/*
 * Keraunos really is zero, which is also what an unwritten scratch reads. The
 * two are told apart by which image is running: a Keraunos build can only be
 * on a Keraunos die, and the Mimir build never accepts it.
 */
#define CHIP_ID_KERAUNOS0 0x00U

/*
 * The SMN sideband manager's D2D-select mux. Aliased because the generated
 * names are too long to use where they are needed.
 *
 * Mimir only, and not merely unused elsewhere: Keraunos reaches its D2D tiles
 * through the HSIO fabric and has no such mux, so the names do not exist in
 * its register map and referring to them would not compile.
 */
#if !defined(CONFIG_SOC_TT_KERAUNOS_SMC)
#define SMN_D2D_SELECT_CLR                                                                         \
	SMN_SMC2SMN_INIU_SIDEBANDMANAGER_D2D_SELECT_MAIN_SIDEBANDMANAGER_A_SIDEBANDMANAGER_D2D_SELECT_MAIN_SIDEBANDMANAGER_FLAGOUTCLR0_REG_ADDR
#define SMN_D2D_SELECT_SET                                                                         \
	SMN_SMC2SMN_INIU_SIDEBANDMANAGER_D2D_SELECT_MAIN_SIDEBANDMANAGER_A_SIDEBANDMANAGER_D2D_SELECT_MAIN_SIDEBANDMANAGER_FLAGOUTSET0_REG_ADDR
#endif

struct link_tile {
	const struct device *dev;
	uintptr_t base;
};

#define TILE(label)                                                                                \
	{                                                                                          \
		.dev = DEVICE_DT_GET(DT_NODELABEL(label)),                                         \
		.base = DT_REG_ADDR(DT_NODELABEL(label)),                                          \
	}

/*
 * Every tile this die has enabled, trained or not. They all come out of reset
 * together: a tile samples its straps at whatever rate is running at the time,
 * and the clock switch that ends that window is chip-wide, so a tile left
 * behind could not be strapped correctly afterwards. Taken from devicetree so
 * that what gets released follows what the overlay turned on -- on Keraunos
 * the tiles behind the other HSIO tiles stay off, and reaching for one of
 * those would hang on an unclocked bus rather than fail.
 */
#define TILE_ENTRY(node_id)                                                                        \
	{                                                                                          \
		.dev = DEVICE_DT_GET(node_id),                                                     \
		.base = DT_REG_ADDR(node_id),                                                      \
	},

static const struct link_tile chip_tiles[] = {DT_FOREACH_STATUS_OKAY(tenstorrent_grendel_d2d,
								     TILE_ENTRY)};

/*
 * Which of them face another die, per chip id.
 */
#if defined(CONFIG_SOC_TT_KERAUNOS_SMC)

/* Keraunos reaches Mimir1 through D2D2, in HSIO tile 2. */
static const struct link_tile tiles_keraunos0[] = {TILE(d2d2)};

#else

/* Mimir0 reaches Mimir1 through D2D1; Mimir1 answers on D2D0. */
static const struct link_tile tiles_mimir0[] = {TILE(d2d1)};

static const struct link_tile tiles_mimir1[] = {
	TILE(d2d0),
#ifdef CONFIG_TT_D2D_TEST_TRAIN_KERAUNOS
	/* The other side of Mimir1, facing Keraunos0's D2D2. */
	TILE(d2d1),
#endif
};

#endif

/*
 * Two waits, with very different shapes, kept apart so a failure says which
 * one ran out.
 *
 * The release is paced by the host: it has to notice both dies waiting before
 * it does anything, so the budget covers a round trip through a poll loop
 * outside this chip and is generous. Training afterwards is a fixed sequence
 * in the firmware rather than a retry loop, and with the PHY bypassed it is
 * short, so its budget is roughly what the drop allows its own poll. Both are
 * in emulated time, which is what the kernel clock counts here and what the
 * hardware actually experiences.
 */
#define RELEASE_TIMEOUT K_SECONDS(5)

/*
 * Training is waited for in slices so progress can be published between them.
 * The product is the budget; the slice is just how often the host hears about
 * it, and wants to stay well under the harness's own patience.
 */
#define TRAIN_SLICE  K_MSEC(10)
#define TRAIN_SLICES 100U

/*
 * Mirrors of the driver's layout, duplicated rather than shared for the same
 * reason the loader suite duplicates them: this is checking what the driver
 * did, and a shared header would let both drift together.
 */
#define CPU_CTRL_OFFSET 0x1800U
#define CPU_CTRL_HALT   0x00010001U
#define CPU_CTRL_START  0x00010000U

/*
 * The scratch registers are the only thing visible from the host while this
 * runs, and the harness prints them on every poll. Publishing the step means a
 * run that stops responding names where it stopped instead of just failing to
 * finish.
 */
#define STEP(step)       WRITE_SCRATCH(3, 0xB1000000U | (step))
#define STEP_RESULT(val) WRITE_SCRATCH(4, (uint32_t)(val))

/*
 * A die that owns two tiles publishes each one separately, into scratch[4] and
 * scratch[6], so a link that stalls can be told from the one beside it that
 * did not. scratch[5] is the chip id and stays out of the way.
 */
#define STEP_RESULT_TILE(tile, val) WRITE_SCRATCH(4 + ((tile) * 2), (uint32_t)(val))

static const struct link_tile *link_tiles;
static size_t link_count;
static uint32_t chip_id;

static const struct link_tile *link_tiles_for(uint32_t id, size_t *count)
{
#if defined(CONFIG_SOC_TT_KERAUNOS_SMC)
	if (id == CHIP_ID_KERAUNOS0) {
		*count = ARRAY_SIZE(tiles_keraunos0);
		return tiles_keraunos0;
	}
#else
	if (id == CHIP_ID_MIMIR0) {
		*count = ARRAY_SIZE(tiles_mimir0);
		return tiles_mimir0;
	}

	if (id == CHIP_ID_MIMIR1) {
		*count = ARRAY_SIZE(tiles_mimir1);
		return tiles_mimir1;
	}
#endif

	*count = 0;
	return NULL;
}

/*
 * Watching for the host's release directly, rather than folding it into the
 * wait for the link, so the two cannot be confused: a Rocket that was never
 * released and a Rocket that ran and failed to train look identical from the
 * link status register alone.
 *
 * The host releases every Rocket in one pass, so a die with two of them waits
 * for the last to move rather than treating them separately.
 */
static int wait_for_release(void)
{
	k_timepoint_t deadline = sys_timepoint_calc(RELEASE_TIMEOUT);

	do {
		size_t running = 0;

		for (size_t i = 0; i < link_count; i++) {
			if (sys_read32(link_tiles[i].base + CPU_CTRL_OFFSET) == CPU_CTRL_START) {
				running++;
			}
		}

		if (running == link_count) {
			return 0;
		}

		k_msleep(1);
	} while (!sys_timepoint_expired(deadline));

	return -ETIMEDOUT;
}

static void *train_setup(void)
{
	chip_id = READ_SCRATCH(15) & CHIP_ID_MASK;
	WRITE_SCRATCH(5, chip_id);

	STEP(1);
	for (size_t i = 0; i < ARRAY_SIZE(chip_tiles); i++) {
		zassert_true(device_is_ready(chip_tiles[i].dev), "%s not ready",
			     chip_tiles[i].dev->name);
	}

	/*
	 * Guessing would be worse than stopping. Bringing up a tile that faces
	 * a neighbour this package does not have fails in exactly the way a
	 * genuine training failure does, so an unset chip id has to be called
	 * out here rather than left to look like a dead link.
	 */
	STEP(2);
	link_tiles = link_tiles_for(chip_id, &link_count);
	zassert_not_null(link_tiles,
			 "scratch[15] low byte is 0x%02x, which is not a die this image knows "
			 "how to be; the harness has not identified this die",
			 chip_id);

	STEP(3);
	zassert_ok(tt_d2d_test_chip_init(), "chip init failed");

	/*
	 * Every tile the die has comes out of reset, not just the ones that
	 * get trained: a tile samples its straps at whatever rate is running
	 * at the time, and the clock switch below ends that window for all of
	 * them at once.
	 */
	STEP(4);
	for (size_t i = 0; i < ARRAY_SIZE(chip_tiles); i++) {
		zassert_ok(tt_d2d_reset_release(chip_tiles[i].dev), "%s reset release failed",
			   chip_tiles[i].dev->name);
	}

	/*
	 * The firmware brings up its PLL against the CCE clock and reports a
	 * lock failure if it is still running off the reference, so this has
	 * to happen before the image is loaded rather than after.
	 */
	STEP(5);
	zassert_ok(tt_d2d_test_chip_clock_up(), "clock switch failed");

	/*
	 * Mimir0 only: point the SMN sideband manager's D2D-select mux at D2D1
	 * so SMC-originated traffic leaves on the tile that faces Mimir1.
	 * Clearing before setting is what the drop does; the flag-out is level
	 * sensitive and may already be set.
	 *
	 * Mimir1 and Keraunos0 are left alone. The mux picks one tile, which
	 * is no use to the die that owns both ends of two links, and the drop
	 * only programs it on Mimir0.
	 */
#if !defined(CONFIG_SOC_TT_KERAUNOS_SMC)
	if (chip_id == CHIP_ID_MIMIR0) {
		STEP(6);
		sys_write32(0x1, SMN_D2D_SELECT_CLR);
		sys_write32(0x1, SMN_D2D_SELECT_SET);
	}
#endif

	STEP(7);

	return NULL;
}

/*
 * One test, because the steps are not independent: there is nothing to check
 * about training except that it happened, and every step before it is setup
 * for the one after. Splitting them would just re-run the bring-up.
 */
ZTEST(tt_d2d_train, test_link_trains)
{
	const uint8_t *img;
	size_t size = 0;
	int ret;

#ifdef D2D_FW_STUB_UNIT
	/*
	 * The generated stand-in is ASCII, not instructions. Nothing will
	 * train against it, and a failure here would say nothing about the
	 * hardware. Build with -DD2D_FW_BIN pointing at the drop's image.
	 */
	ztest_test_skip();
#endif

	STEP(8);
	img = d2d_fw_image(&size);
	zassert_not_null(img, "no firmware image linked in");
	zassert_true(size > 0, "linked firmware image is empty");

	/*
	 * Every tile this die owns is loaded before any of them is handed
	 * over. The host releases all the Rockets in one pass, so a tile left
	 * unloaded at that point would start on whatever is in its SRAM.
	 */
	for (size_t i = 0; i < link_count; i++) {
		const struct link_tile *tile = &link_tiles[i];

		STEP(9);
		ret = tt_d2d_load_fw(tile->dev, img, size);
		STEP_RESULT_TILE(i, ret);
		zassert_ok(ret, "loading %s failed", tile->dev->name);

		STEP(10);
		zassert_equal(sys_read32(tile->base + CPU_CTRL_OFFSET), CPU_CTRL_HALT,
			      "%s should still be halted after a load", tile->dev->name);
	}

	/*
	 * Hand over to the host. It releases these Rockets and the far ones
	 * together once every die has got this far, which is the only way they
	 * come up close enough in time to train.
	 */
	STEP(11);
	WRITE_SCRATCH(0, TS_D2D_WAIT_TRAINING);

	STEP(12);
	zassert_ok(wait_for_release(),
		   "a Rocket on this die was never released. The host does that once every die "
		   "reports TS_D2D_WAIT_TRAINING, so either another die never got there or the "
		   "harness is not driving this run");

	/*
	 * From here the far die has to be doing the same thing, so a timeout
	 * is ambiguous on its own: this end may be fine and the other absent.
	 * That is what the progress code separates -- one that advanced and
	 * stopped is a link problem, one that never moved is this side's
	 * firmware not running at all.
	 *
	 * The wait is sliced so the code can be published as it changes rather
	 * than only once the outcome is known. Under emulation the kernel
	 * clock advances far slower than the wall clock the run is watched on,
	 * so a single wait long enough to be fair to the firmware is also long
	 * enough to be indistinguishable from a wedged core. Republishing each
	 * time round tells the two apart and shows training advancing.
	 *
	 * The tiles are waited on one after another rather than together. They
	 * are already training in parallel -- the Rockets were all released
	 * before any of this -- so the only cost is that the second tile's
	 * progress is published late, and in exchange a stall names the link
	 * it happened on.
	 */
	STEP(13);
	for (size_t i = 0; i < link_count; i++) {
		const struct link_tile *tile = &link_tiles[i];

		for (unsigned int slice = 0; slice < TRAIN_SLICES; slice++) {
			ret = tt_d2d_wait_link(tile->dev, TRAIN_SLICE);
			STEP_RESULT_TILE(i, tt_d2d_progress_code(tile->dev));
			if (ret != -ETIMEDOUT) {
				break;
			}
		}

		zassert_ok(ret,
			   "%s did not train. Check that the die on the other end of it booted "
			   "and reached this point too; neither end can train alone",
			   tile->dev->name);
	}

	/*
	 * Nothing is written here on success: ztest's end-of-run report puts
	 * TS_PASS in scratch[0], which is the token the harness is waiting for.
	 */
	STEP(14);
}

/*
 * A passing run ends with TS_PASS in scratch[0], written by Grendel's
 * TC_END_REPORT override. A failing one writes nothing, which would leave
 * TS_D2D_WAIT_TRAINING sitting there -- the host would go on believing this
 * die is still waiting to be released and poll until it gave up, turning a
 * failure that is already understood into a timeout. Saying so explicitly
 * ends the run on both dies with a verdict instead.
 */
static void train_teardown(void *fixture)
{
	ARG_UNUSED(fixture);

	for (struct ztest_unit_test *t = _ztest_unit_test_list_start; t < _ztest_unit_test_list_end;
	     t++) {
		if (t->stats->fail_count != 0) {
			WRITE_SCRATCH(0, TS_FAIL);
			return;
		}
	}
}

ZTEST_SUITE(tt_d2d_train, NULL, train_setup, NULL, NULL, train_teardown);
