#!/usr/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Single and all Tensix reset smoke and stress tests.
"""

import pytest
import logging
import subprocess
import time

import ttdal.power
import ttdal.telem
from dal_utils import ARC_TILE, noc_broadcast32, noc_read32, noc_write32, smc_message


logger = logging.getLogger(__name__)

TEST_ITERATIONS = 100

TT_SMC_MSG_REINIT_TENSIX = 0x20
TT_SMC_MSG_FORCE_AICLK = 0x33
TT_SMC_MSG_GET_AICLK = 0x34
TT_SMC_MSG_TOGGLE_TENSIX_RESET = 0xAF
TT_SMC_MSG_TOGGLE_SINGLE_TENSIX_RESET = 0xAE

CLOCK_MODE_PPM_FORCED = 2
TENSIX_RISC_RESET_ADDRS = [0x80030040 + i * 4 for i in range(8)]

TENSIX_NOC_Y = range(2, 12)
TRISC0_RESET_PC = 0xFFB12228
SOFT_RESET_0 = 0xFFB121B0
ALL_RISC_SOFT_RESET = 0x47800
PATTERN = 0xA5A5


def _logical_tensix_x_coords(enabled_cols: int) -> list[int]:
    """Return valid logical NOC0 X coordinates for enabled tensix columns.

    Translation fills logical X 1..7 (left half) then 10..16 (right half)
    with good columns in ascending physical NOC0 X order.  Harvested columns
    are pushed to the high end of the right half.
    """
    num_enabled = enabled_cols.bit_count()
    left_count = min(7, num_enabled)
    right_count = max(0, num_enabled - 7)
    return list(range(1, 1 + left_count)) + list(range(10, 10 + right_count))


def _enabled_tensix_tiles(sess) -> list[tuple[int, int]]:
    enabled_cols = sess.telemetry()[ttdal.telem.Tag.EnabledTensixCol]
    assert enabled_cols != 0
    return [
        (x, y) for x in _logical_tensix_x_coords(enabled_cols) for y in TENSIX_NOC_Y
    ]


def _tile_is_reset(tlb, tile: tuple[int, int]) -> bool:
    return (
        noc_read32(tlb, *tile, TRISC0_RESET_PC) == 0
        and noc_read32(tlb, *tile, SOFT_RESET_0) == ALL_RISC_SOFT_RESET
    )


def _others_have_pattern(tlb, tiles: list[tuple[int, int]], target: tuple[int, int]) -> bool:
    return all(
        noc_read32(tlb, *tile, TRISC0_RESET_PC) == PATTERN
        for tile in tiles
        if tile != target
    )


def _format_failed_tiles(failed):
    failed_formatted = "\n".join(
        " " * 13 + ", ".join(repr(t) for t in failed[i : i + 10])
        for i in range(0, len(failed), 10)
    )
    return f"Failed on {len(failed)} tiles:\n{failed_formatted}"


@pytest.mark.smoke
def test_single_tensix_reset(dal_chip):
    """
    Reset every Tensix tile one at a time and check that only the target
    tile is affected. For each tile:

    1. Reset it and check it was cleared and has its RISCs held in soft reset.
    2. Check every *other* tile still holds the pattern.
    """
    sess, tlb = dal_chip
    tiles = _enabled_tensix_tiles(sess)
    sess.power(ttdal.power.Flag.HI)

    # Set pattern before reset
    for tile in tiles:
        noc_write32(tlb, *tile, TRISC0_RESET_PC, PATTERN)

    # Reset each tensix individually and record failures
    failed = []
    for tile in tiles:
        status, _ = smc_message(
            sess, TT_SMC_MSG_TOGGLE_SINGLE_TENSIX_RESET, tile[0] | (tile[1] << 8)
        )
        if (
            status != 0 or
            not _tile_is_reset(tlb, tile) or
            not _others_have_pattern(tlb, tiles, tile)
        ):
            failed.append(tile)

        # Restore pattern on the reset tile before moving to the next
        noc_write32(tlb, *tile, TRISC0_RESET_PC, PATTERN)

    if failed:
        logger.debug(_format_failed_tiles(failed))
    assert not failed


@pytest.mark.smoke
def test_all_tensix_reset(dal_chip):
    """
    Reset all Tensix tiles at once and check that every tile was reset and
    came back with its RISCs held in soft reset.
    """
    sess, tlb = dal_chip
    tiles = _enabled_tensix_tiles(sess)
    sess.power(ttdal.power.Flag.HI)

    # Write pattern to see if it gets cleared on reset
    for tile in tiles:
        noc_write32(tlb, *tile, TRISC0_RESET_PC, PATTERN)

    status, _ = smc_message(sess, TT_SMC_MSG_TOGGLE_TENSIX_RESET)
    assert status == 0

    failed = [tile for tile in tiles if not _tile_is_reset(tlb, tile)]
    if failed:
        logger.debug(_format_failed_tiles(failed))
    assert not failed


@pytest.mark.smoke
def test_legacy_tensix_reset(dal_chip):
    """
    Run the reset sequence host tools used before TOGGLE_TENSIX_RESET did it
    in firmware, and check that repeating those steps on the host is benign.
    """
    sess, tlb = dal_chip
    tiles = _enabled_tensix_tiles(sess)
    sess.power(ttdal.power.Flag.HI)

    # Write pattern to see if it gets cleared on reset
    for tile in tiles:
        noc_write32(tlb, *tile, TRISC0_RESET_PC, PATTERN)

    status, _ = smc_message(sess, TT_SMC_MSG_FORCE_AICLK, 250)
    assert status == 0
    for addr in TENSIX_RISC_RESET_ADDRS:
        noc_write32(tlb, *ARC_TILE, addr, 0)

    status, _ = smc_message(sess, TT_SMC_MSG_TOGGLE_TENSIX_RESET)
    assert status == 0

    # Check the pattern was cleared on reset
    failed = [tile for tile in tiles if noc_read32(tlb, *tile, TRISC0_RESET_PC) != 0]
    if failed:
        logger.debug(_format_failed_tiles(failed))
    assert not failed


def _run_iterations(test_fn, dal_chip, total_tries):
    fail_count = 0
    for i in range(total_tries):
        try:
            test_fn(dal_chip)
        except AssertionError as e:
            logger.error(f"Failed iteration {i}: {e}")
            fail_count += 1

    assert fail_count == 0


@pytest.mark.stress
def test_tensix_reset_stress(dal_chip):
    _run_iterations(test_all_tensix_reset, dal_chip, TEST_ITERATIONS)


@pytest.mark.stress
def test_legacy_tensix_reset_stress(dal_chip):
    _run_iterations(test_legacy_tensix_reset, dal_chip, TEST_ITERATIONS)


@pytest.mark.stress
def test_tensix_reset_single_stress(dal_chip):
    _run_iterations(test_single_tensix_reset, dal_chip, TEST_ITERATIONS)


@pytest.mark.stress
def test_tensix_reset_then_burnin(dal_chip):
    """
    Reset every non-harvested Tensix tile, then run tt-burnin and check
    it exits successfully.
    """
    sess, _ = dal_chip
    tiles = _enabled_tensix_tiles(sess)

    # We want to test the tensix reset message on low power.
    # Set high power after the message to allow NOC read/write to function properly.
    sess.power(ttdal.power.Flag.LO)
    for tile in tiles:
        status, _ = smc_message(
            sess, TT_SMC_MSG_TOGGLE_SINGLE_TENSIX_RESET, tile[0] | (tile[1] << 8)
        )
        assert status == 0, f"Tensix {tile} reset failed with {status}"
    sess.power(ttdal.power.Flag.HI)

    duration = 180

    burnin_process = subprocess.Popen(
        ["tt-burnin", "--no-reset"],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    terminated_early = False
    try:
        end_time = time.time() + duration
        while time.time() < end_time:
            if burnin_process.poll() is not None:
                terminated_early = True
                break
            time.sleep(1.0)
    finally:
        if burnin_process.poll() is None:
            # Send enter key to stop tt-burnin gracefully
            try:
                burnin_process.stdin.write(b"\n")
                burnin_process.stdin.flush()
                burnin_process.wait(timeout=5)
            except (subprocess.TimeoutExpired, BrokenPipeError):
                logger.warning(
                    "tt-burnin did not terminate gracefully, killing process"
                )
                burnin_process.terminate()
                try:
                    burnin_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    burnin_process.kill()
                    burnin_process.wait()

    assert not terminated_early, "tt-burnin terminated early"
    assert burnin_process.returncode == 0, (
        f"tt-burnin failed with {burnin_process.returncode}"
    )
