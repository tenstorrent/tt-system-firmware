#!/usr/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
End-to-end bh-mod and ccfgovr tests.
"""

import logging
import subprocess
from urllib.request import urlretrieve

import ccfgovr_utils as ccfgovr
import pytest
import ttdal.telem
from dal_utils import ARC_TILE, noc_read32, open_chip, wait_arc_boot
from e2e_smoke import (
    _expected_chip_count,
    _prepare_and_launch_dut,
    _skip_boards,
    dirty_reset_test,
    strip_ansi_codes,
)


logger = logging.getLogger(__name__)

TEST_ITERATIONS = 100

ERROR_STATUS0 = 0x80030410
TELEMETRY_DATA_REG_ADDR = 0x80030430
MCUBOOT_HEADER_ADDR = 0x29E000
SPI_SECTOR_SIZE = 0x1000

# This tag does not exist in tt-dal yet, so we need to define it here
TAG_KERNEL_THROTTLER = 75

TDP_PATH = "chip_limits.tdp_limit"

pytestmark = pytest.mark.skipif(
    not ccfgovr.have_bh_mod(), reason="bh-mod not found (PATH or ~/bh-mod)"
)


# Every chip access below opens a tt-dal session for one read and closes it again, so bh-mod and
# tt-smi never finds the device held open across a write or a reset.


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


def _read_tdp(asic_id):
    return _read_telem(asic_id, ttdal.telem.Tag.TdpLimitMax)


def _eth_speed(board_name):
    return 200 if board_name is not None and "galaxy" in board_name.lower() else 400


def _status(asic_id):
    """The bank the firmware applied at its last boot"""
    return ccfgovr.Status(
        _read_telem_raw(asic_id, ccfgovr.TAG_CCFGOVR_STATE),
        _read_telem_raw(asic_id, ccfgovr.TAG_CCFGOVR_SEQ),
    )


def _expected_next(status):
    """
    (bank, seq) the next write lands in: the bank the firmware is not using,
    one seq ahead. Only holds right after a reset, while status still
    reflects the banks on SPI
    """
    if status.bank is None:
        return 0, 0
    seq = (status.seq + 1) & 0xFFFFFFFF
    return 1 - status.bank, 0 if seq == ccfgovr.SEQ_ERASED else seq


def _reset(asic_id, board_name):
    rc = ccfgovr.smi_reset(asic_id, board_name)
    assert rc == 0, f"tt-smi reset failed with {rc}"
    wait_arc_boot(asic_id, timeout=60)


def _write(asic_id, board_name, *args):
    """
    Run a bh-mod write and reset, then check it landed in the bank the
    firmware was not using, one seq ahead, left the previous bank alone as
    the fallback, and was applied. Returns the new status
    """
    before = _status(asic_id)
    bank, seq = _expected_next(before)
    kept = ccfgovr.read_banks(asic_id)[1 - bank]

    rc = ccfgovr.bh_mod_write(asic_id, board_name, *args)
    assert rc == 0, f"bh-mod {' '.join(args)} and reset failed with {rc}"
    wait_arc_boot(asic_id, timeout=60)

    after = _status(asic_id)
    assert (after.bank, after.seq) == (bank, seq), (
        f"expected bank {'AB'[bank]} seq {seq} applied, got {after}"
    )
    if before.bank is not None:
        assert after.bank_state(before.bank) == ccfgovr.BANK_STANDBY, str(after)
    assert ccfgovr.read_banks(asic_id)[1 - bank] == kept, (
        "bh-mod modified the previously applied bank"
    )
    return after


def _inject(asic_id, body):
    """
    Write body as the next override directly to SPI, bypassing bh-mod's
    validation. Returns the bank written
    """
    bank, seq = _expected_next(_status(asic_id))
    addr = ccfgovr.bank_addrs(asic_id)[bank]
    ccfgovr.write_spi(asic_id, addr, ccfgovr.make_bank(seq, body))
    return bank


def _corrupt_cksum(asic_id, bank):
    addr = ccfgovr.bank_addrs(asic_id)[bank] + 16
    cksum = ccfgovr.read_spi(asic_id, addr, 4)
    ccfgovr.write_spi(asic_id, addr, bytes([cksum[0] ^ 1]) + cksum[1:])


def _raw_banks(asic_id, addrs):
    return [ccfgovr.read_spi(asic_id, addr, ccfgovr.BANK_SIZE) for addr in addrs]


@pytest.fixture(autouse=True)
def restore_ccfgovr(asic_id, board_name):
    """
    Clear any override a test left behind. A failure here is only logged, so
    it does not hide the test's own result; the session teardown in conftest.py
    fails the run if an override is still set at the end
    """
    yield
    try:
        if not ccfgovr.leftover_overrides(asic_id):
            return
    except Exception as e:
        logger.warning("Could not read the ccfgovr overrides after the test: %s", e)
    rc = ccfgovr.bh_mod_write(asic_id, board_name, "res", "--all")
    if rc != 0:
        logger.warning("Failed to clear ccfgovr overrides after the test (rc=%d)", rc)


@pytest.mark.smoke
def test_ccfgovr_bh_mod(arc_chip_dut, unlaunched_dut, asic_id, board_name):
    """
    Test override parameters persist across tt-flash.
       1) bh-mod rejects an invalid ETH speed and PCIe gen without touching flash
       2) Override TDP, kernel throttler, ETH speed and PCIe gen via bh-mod
       3) Read back to ensure bh-mod write succeeded
       4) Re-flash FW bundle
       5) Read back to ensure the banks were untouched and overrides persisted

    Negotiated host link gen is slot-dependent and there is no telemetry tag
    for the trained ETH speed, so those two are checked through `bh-mod get`
    and ARC coming back up after the reset, not the links themselves.
    """
    banks = ccfgovr.read_banks(asic_id)
    for field, value in (
        ("eth_property_table.eth_speed_override", 137),
        ("pci0_property_table.max_pcie_speed", 7),
    ):
        rc = ccfgovr.run_bh_mod(asic_id, "set", f"{field}={value}").returncode
        assert rc != 0, f"Expected bh-mod to reject {field}={value}"
    assert ccfgovr.read_banks(asic_id) == banks, "rejected bh-mod set wrote a bank"

    tdp_baseline = _read_tdp(asic_id)

    tdp_target = tdp_baseline - 5
    TARGET_STOP_FREQ = 800  # MHz, within [AICLK_FMIN_MIN=200, AICLK_FMIN_MAX=1400]
    kt_expected = 1 | (TARGET_STOP_FREQ << 16)

    links = {"eth_property_table.eth_speed_override": _eth_speed(board_name)}
    pcie_path = _pcie_ep_speed_path(asic_id)
    if pcie_path is None:
        logger.warning("No PCIe endpoint instance in cmfwcfg, not overriding PCIe gen")
    else:
        links[pcie_path] = 4
    expected = {
        TDP_PATH: tdp_target,
        "feature_enable.kernel_throttler_at_floor_en": True,
        "chip_limits.kernel_throttler_stop_nops_freq": TARGET_STOP_FREQ,
        **links,
    }

    def check_overrides():
        assert _read_tdp(asic_id) == tdp_target
        assert _read_telem_raw(asic_id, TAG_KERNEL_THROTTLER) == kt_expected
        rows = ccfgovr.bh_mod_get(asic_id, *links)
        for path, value in links.items():
            override = rows[path].get("override")
            assert str(override) == str(value), f"{path}: {rows[path]!r}"

    # Persist every override in one bh-mod write.
    status = _write(
        asic_id,
        board_name,
        "set",
        *(f"{path}={str(value).lower()}" for path, value in expected.items()),
    )
    assert status.has_overrides
    assert ccfgovr.active_overrides(asic_id) == expected

    # Verify the overrides were applied by the running firmware.
    check_overrides()

    # Re-flash the firmware bundle.
    banks = ccfgovr.read_banks(asic_id)
    unlaunched_dut.launch()
    wait_arc_boot(asic_id, timeout=60)

    # Confirm the overrides survive.
    assert ccfgovr.read_banks(asic_id) == banks, "tt-flash modified the ccfgovr banks"
    assert _status(asic_id) == status
    check_overrides()


def _pcie_ep_speed_path(asic_id):
    """max_pcie_speed path of the PCIe instance the host talks to, or None"""
    modes = ccfgovr.bh_mod_get(
        asic_id, "pci0_property_table.pcie_mode", "pci1_property_table.pcie_mode"
    )

    def is_ep(entry):
        default = str(entry.get("default", "")).upper()
        return default in ("EP", "1")

    return next(
        (
            f"pci{inst}_property_table.max_pcie_speed"
            for inst in (0, 1)
            if is_ep(modes.get(f"pci{inst}_property_table.pcie_mode", {}))
        ),
        None,
    )


@pytest.mark.smoke
def test_ccfgovr_bypass_cable_fault_override(arc_chip_dut, asic_id, board_name):
    """
    Test cable fault bypass override via bh-mod.
    """
    BYPASS_PATH = "feature_enable.bypass_cable_fault_detection"
    ERROR_STATUS0_CABLE_FAULT = 1 << 1  # BIT(INIT_STAGE_CABLE_FAULT) from init.h

    _write(asic_id, board_name, "set", f"{BYPASS_PATH}=true")

    err = _read_arc32(asic_id, ERROR_STATUS0)
    assert err & ERROR_STATUS0_CABLE_FAULT == 0, f"ERROR_STATUS0=0x{err:08x}"

    row = ccfgovr.bh_mod_get(asic_id, BYPASS_PATH)[BYPASS_PATH]
    assert str(row.get("override")).lower() == "true"


@pytest.mark.stress
def test_ccfgovr_banks_alternate(arc_chip_dut, asic_id, board_name):
    """
    Each bh-mod write must land in the bank the firmware is not using, one
    seq ahead, and leave the applied bank in place as the fallback.
    """
    tdp_baseline = _read_tdp(asic_id)
    for tdp in (tdp_baseline - 5, tdp_baseline - 10, tdp_baseline - 5):
        _write(asic_id, board_name, "set", f"{TDP_PATH}={tdp}")
        assert ccfgovr.active_overrides(asic_id) == {TDP_PATH: tdp}
        assert _read_tdp(asic_id) == tdp


def _write_all(chips, board_name, *args):
    """
    Run a bh-mod write on every chip and reset all of them, returning the
    (bank, seq) each chip's write was expected to land in
    """
    expected = {c: _expected_next(_status(c)) for c in chips}
    rc = ccfgovr.bh_mod_write(None, board_name, *args)
    assert rc == 0, f"bh-mod {' '.join(args)} on every chip and reset failed with {rc}"
    for c in chips:
        wait_arc_boot(c, timeout=60)
    return expected


def _check_all(chips, expected, has_overrides, tdp):
    """Check every chip at once so a failure lists all the chips that are off"""
    errors = []
    for c in chips:
        status = _status(c)
        bank, seq = expected[c]
        if (status.bank, status.seq) != (bank, seq):
            errors.append(
                f"chip {c}: expected bank {'AB'[bank]} seq {seq}, got {status}"
            )
        elif status.has_overrides != has_overrides:
            errors.append(
                f"chip {c}: has_overrides should be {has_overrides}, {status}"
            )
        elif _read_tdp(c) != tdp[c]:
            errors.append(f"chip {c}: TdpLimitMax {_read_tdp(c)}, expected {tdp[c]}")
    assert not errors, "\n".join(errors)


@pytest.mark.stress
def test_ccfgovr_all_chips(arc_chip_dut, board_name):
    """
    bh-mod without -d writes every chip on the host. Each chip's write must
    land in its own standby bank one seq ahead, and every chip must come back
    with it applied after one reset of the whole host (the tray reset on a
    Galaxy). Clearing it the same way must leave every chip clean.
    """
    count = _expected_chip_count(board_name)
    if count is None or count < 2:
        pytest.skip("single-chip board")
    chips = range(count)

    baselines = {c: _read_tdp(c) for c in chips}
    target = min(baselines.values()) - 5
    try:
        expected = _write_all(chips, board_name, "set", f"{TDP_PATH}={target}")
        _check_all(chips, expected, True, {c: target for c in chips})

        expected = _write_all(chips, board_name, "res", "--all")
        _check_all(chips, expected, False, baselines)
    finally:
        # restore_ccfgovr and the session teardown only look at asic_id
        left = [c for c in chips if ccfgovr.leftover_overrides(c)]
        if left:
            logger.warning("Clearing ccfgovr overrides left on chips %s", left)
            ccfgovr.bh_mod_write(None, board_name, "res", "--all")


@pytest.mark.smoke
def test_ccfgovr_torn_write_keeps_applied_bank(arc_chip_dut, asic_id, board_name):
    """
    A bank write cut short by a reset or power loss leaves a newer header
    whose body does not match its CRC. The firmware must keep the bank it
    had, and the next bh-mod write must replace the torn one.
    """
    tdp_baseline = _read_tdp(asic_id)
    status = _write(asic_id, board_name, "set", f"{TDP_PATH}={tdp_baseline - 5}")

    bank, seq = _expected_next(status)
    body = ccfgovr.encode_body({TDP_PATH: tdp_baseline - 10})
    torn = ccfgovr.make_bank(seq, body)[:-4] + b"\xff" * 4
    ccfgovr.write_spi(asic_id, ccfgovr.bank_addrs(asic_id)[bank], torn)
    _reset(asic_id, board_name)

    after = _status(asic_id)
    assert (after.bank, after.seq) == (status.bank, status.seq), str(after)
    assert after.bank_state(bank) == ccfgovr.BANK_REJECTED, str(after)
    assert _read_tdp(asic_id) == tdp_baseline - 5

    _write(asic_id, board_name, "set", f"{TDP_PATH}={tdp_baseline - 15}")
    assert _read_tdp(asic_id) == tdp_baseline - 15


@pytest.mark.stress
def test_ccfgovr_corrupt_bank_falls_back(arc_chip_dut, asic_id, board_name):
    """
    If the applied bank is corrupted, the firmware must fall back to the
    other bank, which holds the override written before it.
    """
    tdp_baseline = _read_tdp(asic_id)
    first = _write(asic_id, board_name, "set", f"{TDP_PATH}={tdp_baseline - 5}")
    second = _write(asic_id, board_name, "set", f"{TDP_PATH}={tdp_baseline - 10}")

    _corrupt_cksum(asic_id, second.bank)
    _reset(asic_id, board_name)

    after = _status(asic_id)
    assert (after.bank, after.seq) == (first.bank, first.seq), str(after)
    assert after.bank_state(second.bank) == ccfgovr.BANK_REJECTED, str(after)
    assert _read_tdp(asic_id) == tdp_baseline - 5


@pytest.mark.stress
def test_ccfgovr_corrupt_empty_bank_resurrects_override(
    arc_chip_dut, asic_id, board_name
):
    """
    `bh-mod res --all` writes a newer empty bank rather than erasing the old
    one, so corrupting the empty bank brings the older override back. This
    pins down that behaviour.
    """
    tdp_baseline = _read_tdp(asic_id)
    first = _write(asic_id, board_name, "set", f"{TDP_PATH}={tdp_baseline - 5}")
    cleared = _write(asic_id, board_name, "res", "--all")
    assert _read_tdp(asic_id) == tdp_baseline

    assert not cleared.has_overrides, str(cleared)

    _corrupt_cksum(asic_id, cleared.bank)
    _reset(asic_id, board_name)

    after = _status(asic_id)
    assert (after.bank, after.seq) == (first.bank, first.seq), str(after)
    assert after.has_overrides, str(after)
    assert _read_tdp(asic_id) == tdp_baseline - 5


def _check_kt_stop_freq_ignored(asic_id, value):
    assert _read_telem_raw(asic_id, TAG_KERNEL_THROTTLER) >> 16 == 0


def _check_kt_enabled(asic_id, value):
    assert _read_telem_raw(asic_id, TAG_KERNEL_THROTTLER) & 1


def _check_tdp_reported(asic_id, value):
    # Telemetry reports the table value as is; the throttler clamps its
    # copy to [50, 500] W
    assert _read_tdp(asic_id) == value


OUT_OF_RANGE = [
    ("chip_limits.tdp_limit", 0, _check_tdp_reported),
    ("chip_limits.tdp_limit", 0xFFFFFFFF, _check_tdp_reported),
    ("chip_limits.max_tdp_limit", 0, None),
    ("chip_limits.max_tdp_limit", 0xFFFFFFFF, None),
    ("chip_limits.kernel_throttler_stop_nops_freq", 1, _check_kt_stop_freq_ignored),
    (
        "chip_limits.kernel_throttler_stop_nops_freq",
        0xFFFFFFFF,
        _check_kt_stop_freq_ignored,
    ),
    ("feature_enable.kernel_throttler_at_floor_en", 2, _check_kt_enabled),
    ("eth_property_table.eth_speed_override", 137, None),
    ("eth_property_table.eth_speed_override", 0xFFFFFFFF, None),
]


def _out_of_range_test(asic_id, board_name, path, value, check=None):
    """
    Write a value bh-mod would refuse straight to SPI, then check ARC boots
    with it applied and bh-mod can clear it again
    """
    tdp_baseline = _read_tdp(asic_id)
    bank = _inject(asic_id, ccfgovr.encode_body({path: value}))
    _reset(asic_id, board_name)

    status = _status(asic_id)
    assert status.bank == bank and status.has_overrides, str(status)
    assert set(ccfgovr.active_overrides(asic_id)) == {path}
    if check is not None:
        check(asic_id, value)

    status = _write(asic_id, board_name, "res", "--all")
    assert not status.has_overrides, str(status)
    assert _read_tdp(asic_id) == tdp_baseline


@pytest.mark.stress
@pytest.mark.parametrize(
    "path,value,check",
    OUT_OF_RANGE,
    ids=[f"{p.split('.')[-1]}={v:#x}" for p, v, _ in OUT_OF_RANGE],
)
def test_ccfgovr_out_of_range_value(
    arc_chip_dut, asic_id, board_name, path, value, check
):
    _out_of_range_test(asic_id, board_name, path, value, check)


# A bad PCIe speed on the instance the host uses can keep the card from
# enumerating, which would end the rest of the run
@pytest.mark.stress
@pytest.mark.parametrize("value", [0, 6, 0xFFFFFFFF], ids=lambda v: f"{v:#x}")
def test_ccfgovr_out_of_range_pcie_speed(arc_chip_dut, asic_id, board_name, value):
    path = _pcie_ep_speed_path(asic_id)
    if path is None:
        pytest.skip("no PCIe endpoint instance in cmfwcfg")
    _out_of_range_test(asic_id, board_name, path, value)


@pytest.mark.stress
def test_ccfgovr_cleared_from_recovery(
    unlaunched_dut, arc_chip_dut, asic_id, board_name
):
    """
    The recovery image ignores ccfgovr, so an override that breaks the main
    firmware can always be cleared from it. Boot recovery with an override
    set, clear it there with bh-mod, then reflash and check the main
    firmware comes up without it.
    """
    if _expected_chip_count(board_name) not in (None, 1):
        # Every ASIC on a multi-chip card would have to be staged into
        # recovery for tt-flash to accept it, see test_mcuboot
        pytest.skip("multi-chip board")

    tdp_baseline = _read_tdp(asic_id)
    _write(asic_id, board_name, "set", f"{TDP_PATH}={tdp_baseline - 5}")

    ccfgovr.write_spi(asic_id, MCUBOOT_HEADER_ADDR, b"\xff" * SPI_SECTOR_SIZE)
    try:
        # tt-smi fails here since recovery has no telemetry, but it still
        # resets the chip
        ccfgovr.smi_reset(asic_id, board_name)
        ccfgovr.wait_fw_ready(asic_id, ccfgovr.FW_ID_SMC_RECOVERY)

        result = ccfgovr.run_bh_mod(asic_id, "res", "--all")
        assert result.returncode == 0, (
            f"bh-mod res --all failed in recovery: {result.stderr}"
        )
        assert ccfgovr.active_overrides(asic_id) == {}
    finally:
        unlaunched_dut.launch()
        wait_arc_boot(asic_id, timeout=60)

    status = _status(asic_id)
    assert not status.has_overrides, str(status)
    assert _read_tdp(asic_id) == tdp_baseline


@pytest.mark.stress
def test_ccfgovr_survives_dmc_reset(arc_chip_dut, asic_id, board_name):
    """
    A DMC reset the SMC did not ask for (as after a NOC hang) must bring the
    firmware back with the same bank applied.
    """
    if board_name is not None and _skip_boards(board_name):
        pytest.skip("no STLink for an OpenOCD dirty reset on this board")

    tdp_target = _read_tdp(asic_id) - 5
    status = _write(asic_id, board_name, "set", f"{TDP_PATH}={tdp_target}")
    banks = ccfgovr.read_banks(asic_id)

    if not dirty_reset_test():
        # The dirty reset itself is allowed the odd failure, see test_dirty_reset
        logger.warning("DMC dirty reset failed, retrying once")
        assert dirty_reset_test(), "DMC dirty reset failed twice"
    wait_arc_boot(asic_id, timeout=60)

    assert ccfgovr.read_banks(asic_id) == banks
    assert _status(asic_id) == status
    assert _read_tdp(asic_id) == tdp_target


def _set_overrides(asic_id, board_name, tdp, speed):
    """
    Write the TDP and ETH speed overrides in one bh-mod call, reset the chip
    with tt-smi, then check ARC came back with both applied.
    """
    _write(
        asic_id,
        board_name,
        "set",
        f"{TDP_PATH}={tdp}",
        f"eth_property_table.eth_speed_override={speed}",
    )
    assert ccfgovr.active_overrides(asic_id) == {
        TDP_PATH: tdp,
        "eth_property_table.eth_speed_override": speed,
    }
    assert _read_tdp(asic_id) == tdp


@pytest.mark.stress
def test_bh_mod_override_stress(arc_chip_dut, asic_id, board_name):
    """
    Write alternating TDP limit and ETH speed overrides with bh-mod repeatedly,
    and check that ARC comes back with both applied after every write.
    """
    tdp_baseline = _read_tdp(asic_id)
    tdp_targets = (tdp_baseline - 5, tdp_baseline - 10)
    speed = _eth_speed(board_name)

    fail_count = 0
    for i in range(TEST_ITERATIONS):
        try:
            _set_overrides(asic_id, board_name, tdp_targets[i % 2], speed)
        except Exception as e:
            logger.error(f"Failed iteration {i}: {e!r}")
            fail_count += 1

    assert fail_count == 0, f"{fail_count}/{TEST_ITERATIONS} iterations failed"


RELEASE_URL = (
    "https://github.com/tenstorrent/tt-system-firmware/releases/download/"
    "v{0}/fw_pack-{0}.fwbundle"
)


@pytest.mark.stress
@pytest.mark.parametrize(
    "version,applies_tdp",
    [
        ("19.10.0", False),  # predates ccfgovr
        ("19.16.0", True),  # has ccfgovr, but not max_tdp_limit
    ],
)
def test_ccfgovr_across_downgrade(
    arc_chip_dut, unlaunched_dut, tmp_path, asic_id, board_name, version, applies_tdp
):
    """
    Downgrade with overrides set, then upgrade back. tt-flash must leave the
    banks alone in both directions, the older firmware must boot with the
    subset of fields it knows, and the current firmware must apply the
    whole override again.
    """
    if board_name is None:
        pytest.skip("requires --board")

    tdp_target = _read_tdp(asic_id) - 5
    status = _write(
        asic_id,
        board_name,
        "set",
        f"{TDP_PATH}={tdp_target}",
        f"chip_limits.max_tdp_limit={tdp_target + 10}",
    )
    # Older firmware has no ccfgovr boot-fs entries to look the banks up by
    addrs = ccfgovr.bank_addrs(asic_id)
    banks = _raw_banks(asic_id, addrs)

    bundle = tmp_path / f"fw_pack-{version}.fwbundle"
    urlretrieve(RELEASE_URL.format(version), bundle)
    result = subprocess.run(
        ["tt-flash", str(bundle), "--force", "--allow-major-downgrades"],
        capture_output=True,
        text=True,
    )
    logger.info(f"tt-flash output: {result.stdout}")
    assert "FLASH SUCCESS" in strip_ansi_codes(result.stdout)
    wait_arc_boot(asic_id, timeout=60)

    assert _raw_banks(asic_id, addrs) == banks, f"flashing {version} modified the banks"
    tdp = _read_tdp(asic_id)
    logger.info("TdpLimitMax on %s with tdp_limit=%d set: %d", version, tdp_target, tdp)
    if applies_tdp:
        assert tdp == tdp_target, f"{version} did not apply the override"

    _prepare_and_launch_dut(unlaunched_dut, board_name=board_name, asic_id=asic_id)

    assert _raw_banks(asic_id, addrs) == banks, "upgrading modified the banks"
    assert _status(asic_id) == status
    assert _read_tdp(asic_id) == tdp_target
