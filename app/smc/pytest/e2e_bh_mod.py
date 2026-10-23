#!/usr/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
End-to-end bh-mod tests.

All bh-mod commands are immediately followed by a tt-smi reset as stated in the usage guides.
"""

import json
import logging
import os
import shutil
import subprocess

import pytest
import ttdal.telem
from dal_utils import ARC_TILE, noc_read32, open_chip, wait_arc_boot


logger = logging.getLogger(__name__)

MAX_TEST_ITERATIONS = 1

ERROR_STATUS0 = 0x80030410
TELEMETRY_DATA_REG_ADDR = 0x80030430

# This tag does not exist in tt-dal yet, so we need to define it here
TAG_KERNEL_THROTTLER = 75

BH_MOD = shutil.which("bh-mod") or os.path.expanduser("~/bh-mod")

pytestmark = pytest.mark.skipif(
    not os.path.isfile(BH_MOD), reason="bh-mod not found (PATH or ~/bh-mod)"
)


# Every chip access below opens a tt-dal session for one read and closes it
# again, so bh-mod and tt-smi never find the device held open across a write
# or a reset.


def _read_arc32(asic_id, addr):
    """Read a 32-bit ARC register over the NOC"""
    with open_chip(asic_id) as (_, tlb):
        return noc_read32(tlb, *ARC_TILE, addr)


def _read_telem(asic_id, tag):
    """Read one named telemetry tag from a fresh snapshot"""
    with open_chip(asic_id) as (sess, _):
        return sess.telemetry()[tag]


def _read_telem_raw(asic_id, idx):
    """Read telemetry entry idx from the table in ARC memory"""
    with open_chip(asic_id) as (_, tlb):
        table = noc_read32(tlb, *ARC_TILE, TELEMETRY_DATA_REG_ADDR)
        return noc_read32(tlb, *ARC_TILE, table + idx * 4)


def _run_bh_mod(*args):
    return subprocess.run([BH_MOD, *args], capture_output=True, check=False)


def _bh_mod_write(board_name, *args):
    """
    Run a bh-mod write, then reset the chip with tt-smi.
    Returns the result of whichever command failed, else the bh-mod result.
    """
    result = _run_bh_mod(*args)
    if result.returncode != 0:
        return result

    if board_name is not None and "galaxy" in board_name.lower():
        cmd = ["tt-smi", "-glx_reset", "--eth_train_skip"]
    else:
        cmd = ["tt-smi", "-r", "--eth_train_skip"]
    reset = subprocess.run(cmd, capture_output=True, check=False)

    return reset.returncode if reset.returncode != 0 else reset.returncode


@pytest.fixture(autouse=True)
def restore_ccfgovr(board_name):
    """Restore ccfgovr after each test."""
    yield
    rc = _bh_mod_write(board_name, "res", "--all")
    assert rc == 0


@pytest.mark.smoke
def test_ccfgovr_bh_mod(unlaunched_dut, asic_id, board_name):
    """
    Test override parameters persist across tt-flash.
       1) Overrite TDP and kernel throttler via bh-mod
       2) Read back to ensure bh-mod write succeeded
       3) Re-flash FW bundle
       4) Read back to ensure overrides persisted
    """
    tdp_baseline = _read_telem(asic_id, ttdal.telem.Tag.TdpLimitMax)

    tdp_target = tdp_baseline - 5
    TARGET_STOP_FREQ = 800  # MHz, within [AICLK_FMIN_MIN=200, AICLK_FMIN_MAX=1400]
    kt_expected = 1 | (TARGET_STOP_FREQ << 16)

    # Persist overrides for both the TDP limit and the kernel throttler config.
    rc = _bh_mod_write(
        board_name,
        "set",
        f"chip_limits.tdp_limit={tdp_target}",
        "feature_enable.kernel_throttler_at_floor_en=true",
        f"chip_limits.kernel_throttler_stop_nops_freq={TARGET_STOP_FREQ}",
    )
    assert rc == 0

    # Verify the overrides were applied by the running firmware.
    measured_tdp = _read_telem(asic_id, ttdal.telem.Tag.TdpLimitMax)
    assert measured_tdp == tdp_target
    measured_kt = _read_telem_raw(asic_id, TAG_KERNEL_THROTTLER)
    assert measured_kt == kt_expected

    # Re-flash the firmware bundle.
    unlaunched_dut.launch()
    wait_arc_boot(asic_id, timeout=60)

    # Confirm the overrides survive.
    measured_tdp_after_flash = _read_telem(asic_id, ttdal.telem.Tag.TdpLimitMax)
    assert measured_tdp_after_flash == tdp_target
    measured_kt_after_flash = _read_telem_raw(asic_id, TAG_KERNEL_THROTTLER)
    assert measured_kt_after_flash == kt_expected


@pytest.mark.smoke
def test_ccfgovr_eth_speed_override(arc_chip_dut, asic_id, board_name):
    """
    Test ETH link speed override via bh-mod.
       1) bh-mod rejects an invalid speed.
       2) bh-mod succesfully writes a valid ETH speed and tt-smi reset brings back SMC
    """
    is_galaxy = board_name is not None and "galaxy" in board_name.lower()
    speed = 200 if is_galaxy else 400

    rc = _run_bh_mod("set", "eth_property_table.eth_speed_override=137").returncode
    assert rc != 0, "Expected bh-mod to reject ETH speed of 137"

    # There is no telemetry tag for the trained ETH speed, so just assert the operating succeeded
    rc = _bh_mod_write(
        board_name, "set", f"eth_property_table.eth_speed_override={speed}"
    )
    assert rc == 0


@pytest.mark.smoke
def test_ccfgovr_pcie_gen_override(arc_chip_dut, asic_id, board_name):
    """
    Test PCIe gen override via bh-mod.

    Negotiated host link gen is slot-dependent, so this test does not
    require a Gen 5 slot. It drives the paths that would fail if the
    override were mis-handled:

    - `bh-mod` rejects a generation the SerDes firmware does not accept
      before touching flash.
    - Writing a supported generation on the EP instance and resetting the
      chip with tt-smi leaves ARC coming back up, with `bh-mod get`
      showing the override.
    """
    rc = _run_bh_mod("set", "pci0_property_table.max_pcie_speed=7")
    assert rc != 0, "Expected bh-mod to reject PCIe speed of 7"

    listing = _run_bh_mod(
        "get",
        "-f",
        "json",
        "pci0_property_table.pcie_mode",
        "pci1_property_table.pcie_mode",
    )
    assert listing.returncode == 0
    modes = json.loads(listing.stdout.decode())

    def is_ep(entry):
        default = str(entry.get("default", "")).upper()
        return default in ("EP", "1")

    path = next(
        (
            f"pci{inst}_property_table.max_pcie_speed"
            for inst in (0, 1)
            if is_ep(modes.get(f"pci{inst}_property_table.pcie_mode", {}))
        ),
        None,
    )
    if path is None:
        pytest.skip("no PCIe endpoint instance in cmfwcfg")

    rc = _bh_mod_write(board_name, "set", f"{path}=4")
    assert rc == 0

    verify = _run_bh_mod("get", "-f", "json", path)
    assert verify.returncode == 0

    row = json.loads(verify.stdout.decode())[path]
    assert str(row.get("override")) == "4", f"expected override {path}=4, got {row!r}"


@pytest.mark.smoke
def test_ccfgovr_bypass_cable_fault_override(arc_chip_dut, asic_id, board_name):
    """
    Test cable fault bypass override via bh-mod.
    """
    # Pin bh-mod to the DUT. Without `-d` it acts on every chip it can see,
    # so on dual-chip boards (P300) `set`/`res` would write both ASICs, and
    # `get -f json` keys its output by chip id instead of by field.
    dev = ("-d", f"/dev/tenstorrent/{asic_id}")

    BYPASS_PATH = "feature_enable.bypass_cable_fault_detection"
    ERROR_STATUS0_CABLE_FAULT = 1 << 1 # BIT(INIT_STAGE_CABLE_FAULT) from init.h

    rc = _bh_mod_write(board_name, *dev, "set", f"{BYPASS_PATH}=true")
    assert rc == 0

    err = _read_arc32(asic_id, ERROR_STATUS0)
    assert err & ERROR_STATUS0_CABLE_FAULT == 0, f"ERROR_STATUS0=0x{err:08x}"

    verify = _run_bh_mod(*dev, "get", "-f", "json", BYPASS_PATH)
    assert verify.returncode == 0

    row = json.loads(verify.stdout.decode())[BYPASS_PATH]
    assert str(row.get("override")).lower() == "true"


def _set_overrides(asic_id, board_name, tdp, speed):
    """
    Write the TDP and ETH speed overrides in one bh-mod call, reset the chip
    with tt-smi, then check ARC came back with both applied.
    """
    rc = _bh_mod_write(
        board_name,
        "set",
        f"chip_limits.tdp_limit={tdp}",
        f"eth_property_table.eth_speed_override={speed}",
    )
    assert rc == 0

    measured_tdp = _read_telem(asic_id, ttdal.telem.Tag.TdpLimitMax)
    assert measured_tdp == tdp

    verify = _run_bh_mod("get", "-f", "json", "eth_property_table.eth_speed_override")
    assert verify.returncode == 0

    row = json.loads(verify.stdout.decode())["eth_property_table.eth_speed_override"]
    assert str(row.get("override")) == str(speed)


@pytest.mark.stress
def test_bh_mod_override_stress(arc_chip_dut, asic_id, board_name):
    """
    Write alternating TDP limit and ETH speed overrides with bh-mod repeatedly,
    and check that ARC comes back with both applied after every write.
    """
    tdp_baseline = _read_telem(asic_id, ttdal.telem.Tag.TdpLimitMax)
    tdp_targets = (tdp_baseline - 5, tdp_baseline - 10)

    is_galaxy = board_name is not None and "galaxy" in board_name.lower()
    speed = 200 if is_galaxy else 400

    fail_count = 0
    for i in range(MAX_TEST_ITERATIONS):
        try:
            _set_overrides(asic_id, board_name, tdp_targets[i % 2], speed)
        except AssertionError as e:
            logger.error(f"Failed iteration {i}: {e}")
            fail_count += 1

    assert fail_count == 0
