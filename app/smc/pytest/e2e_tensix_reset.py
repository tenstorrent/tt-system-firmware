#!/usr/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Single and all Tensix reset smoke and stress tests.
"""

import pytest

import ttdal.power
import ttdal.telem
from common import _run_iterations, _run_tt_burnin
from dal_utils import (
    ARC_TILE,
    noc_read32,
    noc_write32,
    smc_message,
)


TEST_ITERATIONS = 1

TT_SMC_MSG_REINIT_TENSIX = 0x20
TT_SMC_MSG_FORCE_AICLK = 0x33
TT_SMC_MSG_TOGGLE_TENSIX_RESET = 0xAF
TT_SMC_MSG_TOGGLE_SINGLE_TENSIX_RESET = 0xAE

TENSIX_RISC_RESET_ADDRS = [0x80030040 + i * 4 for i in range(8)]

TENSIX_NOC_Y = range(2, 12)
TRISC0_RESET_PC = 0xFFB12228
SOFT_RESET_0 = 0xFFB121B0
ALL_RISC_SOFT_RESET = 0x47800
NIU_CFG_0 = 0xFFB20100
NIU_CFG_0_TILE_CLK_OFF = 1 << 12
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


def _tile_is_clock_gated(tlb, tile: tuple[int, int]) -> bool:
    return bool(noc_read32(tlb, *tile, NIU_CFG_0) & NIU_CFG_0_TILE_CLK_OFF)


def _risc_reset_state(tlb) -> list[int]:
    return [noc_read32(tlb, *ARC_TILE, addr) for addr in TENSIX_RISC_RESET_ADDRS]


def _others_have_pattern(
    tlb, tiles: list[tuple[int, int]], target: tuple[int, int]
) -> bool:
    return all(
        noc_read32(tlb, *tile, TRISC0_RESET_PC) == PATTERN
        for tile in tiles
        if tile != target
    )


@pytest.mark.smoke
def test_single_tensix_reset(dal_chip):
    """
    Reset every Tensix tile one at a time and check that only the target
    tile is affected. For each tile,
        1. Reset and check pattern is cleared and its RISCs held in soft reset.
        2. Check every other tile still holds the pattern.
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
            status != 0
            or not _tile_is_reset(tlb, tile)
            or not _others_have_pattern(tlb, tiles, tile)
        ):
            failed.append(tile)

        # Restore pattern on the reset tile before moving to the next
        noc_write32(tlb, *tile, TRISC0_RESET_PC, PATTERN)

    assert not failed, f"{failed}"


@pytest.mark.smoke
def test_all_tensix_reset(dal_chip):
    """
    Reset all Tensix tiles at once and check that every tile was reset.
        1. Write a pattern to every tile.
        2. Send TOGGLE_TENSIX_RESET and check the RISC resets were released.
        3. Check every tile was cleared and has its RISCs held in soft reset.
    """
    sess, tlb = dal_chip
    tiles = _enabled_tensix_tiles(sess)
    sess.power(ttdal.power.Flag.HI)
    risc_reset = _risc_reset_state(tlb)

    # Write pattern to see if it gets cleared on reset
    for tile in tiles:
        noc_write32(tlb, *tile, TRISC0_RESET_PC, PATTERN)

    status, _ = smc_message(sess, TT_SMC_MSG_TOGGLE_TENSIX_RESET)
    assert status == 0
    assert _risc_reset_state(tlb) == risc_reset

    failed = [tile for tile in tiles if not _tile_is_reset(tlb, tile)]
    assert not failed, f"{failed}"


@pytest.mark.smoke
def test_all_tensix_reset_low_power(dal_chip):
    """
    Reset all Tensix tiles while they are clock-gated in low power.
        1. Write a pattern to every tile, then switch to low power.
        2. Send TOGGLE_TENSIX_RESET.
        3. Check every tile is still clock-gated and the RISC resets were released.
        4. Switch to high power and check every tile was cleared and has its
           RISCs held in soft reset.
    """
    sess, tlb = dal_chip
    tiles = _enabled_tensix_tiles(sess)
    sess.power(ttdal.power.Flag.HI)
    risc_reset = _risc_reset_state(tlb)

    # Write pattern to see if it gets cleared on reset
    for tile in tiles:
        noc_write32(tlb, *tile, TRISC0_RESET_PC, PATTERN)

    sess.power(ttdal.power.Flag.LO)
    assert all(_tile_is_clock_gated(tlb, tile) for tile in tiles)

    status, _ = smc_message(sess, TT_SMC_MSG_TOGGLE_TENSIX_RESET)
    assert status == 0

    ungated = [tile for tile in tiles if not _tile_is_clock_gated(tlb, tile)]
    assert not ungated, f"{ungated}"
    assert _risc_reset_state(tlb) == risc_reset

    # Tile registers are only accessible with the tile clocks running
    sess.power(ttdal.power.Flag.HI)
    failed = [tile for tile in tiles if not _tile_is_reset(tlb, tile)]
    assert not failed, f"{failed}"


@pytest.mark.smoke
def test_reinit_tensix_low_power(dal_chip):
    """
    Re-initialize Tensix NOC programming while the tiles are clock-gated in
    low power. REINIT_TENSIX does not reset tiles.
        1. Write a pattern to every tile, then switch to low power.
        2. Send REINIT_TENSIX.
        3. Check every tile is still clock-gated.
        4. Switch to high power and check every tile still holds the pattern.
    """
    sess, tlb = dal_chip
    tiles = _enabled_tensix_tiles(sess)
    sess.power(ttdal.power.Flag.HI)

    # REINIT_TENSIX does not reset tiles, so the pattern should survive
    for tile in tiles:
        noc_write32(tlb, *tile, TRISC0_RESET_PC, PATTERN)

    sess.power(ttdal.power.Flag.LO)
    assert all(_tile_is_clock_gated(tlb, tile) for tile in tiles)

    status, _ = smc_message(sess, TT_SMC_MSG_REINIT_TENSIX)
    assert status == 0

    ungated = [tile for tile in tiles if not _tile_is_clock_gated(tlb, tile)]
    assert not ungated, f"{ungated}"

    # Tile registers are only accessible with the tile clocks running
    sess.power(ttdal.power.Flag.HI)
    failed = [
        tile for tile in tiles if noc_read32(tlb, *tile, TRISC0_RESET_PC) != PATTERN
    ]
    assert not failed, f"{failed}"


@pytest.mark.smoke
def test_legacy_tensix_reset(dal_chip):
    """
    Run the reset steps host tools did before TOGGLE_TENSIX_RESET moved them
    into firmware, and check that repeating them is benign.
        1. Write a pattern to every tile.
        2. Force AICLK to 250 MHz and assert the RISC resets from the host.
        3. Send TOGGLE_TENSIX_RESET.
        4. Check every tile was cleared.
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
    assert not failed, f"{failed}"


@pytest.mark.stress
def test_tensix_reset_stress(dal_chip):
    _run_iterations(test_all_tensix_reset, dal_chip, TEST_ITERATIONS)


@pytest.mark.stress
def test_tensix_reset_low_power_stress(dal_chip):
    _run_iterations(test_all_tensix_reset_low_power, dal_chip, TEST_ITERATIONS)


@pytest.mark.stress
def test_reinit_tensix_low_power_stress(dal_chip):
    _run_iterations(test_reinit_tensix_low_power, dal_chip, TEST_ITERATIONS)


@pytest.mark.stress
def test_legacy_tensix_reset_stress(dal_chip):
    _run_iterations(test_legacy_tensix_reset, dal_chip, TEST_ITERATIONS)


@pytest.mark.stress
def test_tensix_reset_single_stress(dal_chip):
    _run_iterations(test_single_tensix_reset, dal_chip, TEST_ITERATIONS)


@pytest.mark.stress
def test_tensix_reset_then_burnin(dal_chip):
    """
    Reset every Tensix tile one at a time in low power, then run tt-burnin.
        1. Switch to low power.
        2. Send TOGGLE_SINGLE_TENSIX_RESET for every tile.
        3. Switch to high power.
        4. Run tt-burnin and check it exits successfully.
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

    _run_tt_burnin()


@pytest.mark.stress
def test_all_tensix_reset_then_burnin(dal_chip):
    """
    Reset all Tensix tiles at once in low power, then run tt-burnin.
        1. Switch to low power.
        2. Send TOGGLE_TENSIX_RESET.
        3. Switch to high power.
        4. Run tt-burnin and check it exits successfully.
    """
    sess, _ = dal_chip

    # We want to test the tensix reset message on low power.
    # Set high power after the message to allow NOC read/write to function properly.
    sess.power(ttdal.power.Flag.LO)
    status, _ = smc_message(sess, TT_SMC_MSG_TOGGLE_TENSIX_RESET)
    assert status == 0, f"Tensix reset failed with {status}"
    sess.power(ttdal.power.Flag.HI)

    _run_tt_burnin()
