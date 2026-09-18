#!/bin/env python3

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import logging
import os
import time
import subprocess
from pathlib import Path
import pyluwen
import pytest

from e2e_smoke import (
    dirty_reset_test,
    smi_reset_test,
    smi_reset_with_eth,
    arc_watchdog_test,
    pcie_fw_load_time_test,
    upgrade_from_version_test,
    pvt_comprehensive_test,
    voltage_monitors_test,
    process_detectors_test,
    temperature_sensors_test,
    power_state_toggle_test,
    _logical_tensix_x_coords,
)

# Needed to keep ruff from complaining about this "unused import"
# ruff: noqa: F811
from e2e_smoke import arc_chip_dut, launched_arc_dut  # noqa: F401

try:
    from e2e_smoke import unlaunched_dut  # noqa: F401
except ImportError:
    pass  # we should have it from twister

# ccfgovr (bh-mod override) helpers + telemetry tags shared with e2e_smoke.
from e2e_smoke import (
    read_telem,
    wait_arc_boot,
    _require_bh_mod,
    _bh_mod_set,
    _bh_mod_res,
    _read_tdp_limit,
    _reset_ccfgovr,
    TAG_INPUT_POWER,
    TAG_KERNEL_THROTTLER,
)


logger = logging.getLogger(__name__)

SCRIPT_DIR = Path(os.path.dirname(os.path.abspath(__file__)))


def _skip_boards() -> bool:
    return os.getenv("BOARD") in (
        "bh-galaxy",
        "bh-galaxy-revc",
        "loudbox",
        "quietbox2",
    )


# Constant memory addresses we can read from SMC
PING_DMFW_DURATION_REG_ADDR = 0x80030448

# ARC messages
TT_SMC_MSG_PING_DM = 0xC0
TT_SMC_MSG_READ_TS = 0x1B
TT_SMC_MSG_TEST = 0x90
TT_SMC_MSG_TOGGLE_SINGLE_TENSIX_RESET = 0xAE

# Lower this number if testing local changes, so that tests run faster.
MAX_TEST_ITERATIONS = 1000

NUM_PD = 16
NUM_VM = 8
NUM_TS = 8


def report_results(test_name, fail_count, total_tries):
    """
    Helper function to log the results of a test. This uses a
    consistent format so that twister can parse the results
    """
    logger.info(f"{test_name} completed. Failed {fail_count}/{total_tries} times.")


def tt_smi_reset():
    """
    Resets the SMC using tt-smi
    """
    smi_reset_cmd = "tt-smi -r --eth_train_skip"
    smi_reset_result = subprocess.run(
        smi_reset_cmd.split(), capture_output=True, check=False
    ).returncode
    return smi_reset_result


@pytest.mark.skipif(
    "os.getenv('BOARD') in ('bh-galaxy', 'bh-galaxy-revc', 'loudbox', 'quietbox2')",
    reason="Galaxy lacks a DMC; Loudbox/Quietbox2 excluded from DMC-dependent stress tests",
)
def test_arc_watchdog(arc_chip_dut, asic_id):
    """
    Validates that the DMC firmware watchdog for the ARC will correctly
    reset the chip
    """
    # todo: find better way to get test name
    test_name = "ARC watchdog test"
    total_tries = min(MAX_TEST_ITERATIONS, 100)
    fail_count = 0
    failure_fail_count = 0

    for i in range(total_tries):
        if i % 10 == 0:
            logger.info(f"{test_name} iteration {i}/{total_tries}")

        result = arc_watchdog_test(asic_id)
        if not result:
            logger.warning(f"{test_name} failed on iteration {i}")
            fail_count += 1

    report_results(test_name, fail_count, total_tries)
    assert fail_count <= failure_fail_count, (
        f"{test_name} failed {fail_count}/{total_tries} times."
    )


@pytest.mark.skipif(
    "os.getenv('BOARD') in ('bh-galaxy', 'bh-galaxy-revc', 'loudbox', 'quietbox2')",
    reason="Galaxy lacks a DMC; Loudbox/Quietbox2 excluded from DMC-dependent stress tests",
)
def test_pcie_fw_load_time(arc_chip_dut, asic_id):
    """
    Checks PCIe firmware load time is within 40ms.
    This test needs to be run after production reset.
    """
    # todo: find better way to get test name
    test_name = "PCIe firmware load time test"
    total_tries = min(MAX_TEST_ITERATIONS, 10)
    fail_count = 0
    failure_fail_count = 0

    for i in range(total_tries):
        logger.info(
            f"Starting PCIe firmware load time test iteration {i}/{total_tries}"
        )
        # Reset the SMC to ensure we have a clean state
        if tt_smi_reset() != 0:
            logger.warning(f"tt-smi reset failed on iteration {i}")
            fail_count += 1
            continue
        result = pcie_fw_load_time_test(asic_id)
        if not result:
            logger.warning(f"PCIe firmware load time test failed on iteration {i}")
            fail_count += 1

    report_results(test_name, fail_count, total_tries)
    assert fail_count <= failure_fail_count, (
        f"{test_name} failed {fail_count}/{total_tries} times."
    )


def test_smi_reset(arc_chip_dut, asic_id):
    """
    Checks that tt-smi resets are working successfully
    """
    # todo: find better way to get test name
    test_name = "tt-smi reset test"
    # todo: increase test count back to 1000. This was dropped to support
    # new tt-smi, which has a longer reset duration due to using UMD
    # health checks
    total_tries = min(MAX_TEST_ITERATIONS, 200)
    fail_count = 0
    failure_fail_count = total_tries // 100
    dmfw_ping_avg = 0
    dmfw_ping_max = 0
    for i in range(total_tries):
        if i % 10 == 0:
            logger.info(f"{test_name} iteration {i}/{total_tries}")

        result = smi_reset_test(asic_id)

        if not result:
            logger.warning(f"tt-smi reset failed on iteration {i}")
            fail_count += 1
            continue

        arc_chip = pyluwen.detect_chips()[asic_id]
        if not _skip_boards():
            response = arc_chip.arc_msg(TT_SMC_MSG_PING_DM, True, False, 0, 0, 1000)
            if response[0] != 1 or response[1] != 0:
                logger.warning(f"Ping failed on iteration {i}")
                fail_count += 1
            duration = arc_chip.axi_read32(PING_DMFW_DURATION_REG_ADDR)
            dmfw_ping_avg += duration / total_tries
            dmfw_ping_max = max(dmfw_ping_max, duration)
        else:
            # Just check ARC ping, Galaxy lacks a DMC
            response = arc_chip.arc_msg(TT_SMC_MSG_TEST, True, False, 0, 0, 1000)
            if response[0] != 1 or response[1] != 0:
                logger.warning(f"Ping failed on iteration {i}")
                fail_count += 1
        # Delete arc_chip so we don't hold an open FD
        del arc_chip

    if not _skip_boards():
        logger.info(
            f"Average DMFW ping time (after reset): {dmfw_ping_avg:.2f} ms, "
            f"Max DMFW ping time (after reset): {dmfw_ping_max:.2f} ms."
        )

    report_results(test_name, fail_count, total_tries)
    assert fail_count <= failure_fail_count, (
        f"{test_name} failed {fail_count}/{total_tries} times."
    )


def test_smi_reset_with_eth(arc_chip_dut, asic_id):
    """
    Checks that tt-smi resets with ethernet training are working successfully
    """
    # Ethernet training takes significantly longer, so we reduce the number of
    # iterations to keep test runtime reasonable, while still providing some confidence in stability
    total_tries = 10
    fail_count = 0
    for i in range(total_tries):
        logger.info(f"Iteration {i}:")
        result = smi_reset_with_eth(asic_id)

        if not result:
            logger.warning(
                f"tt-smi reset with ethernet training failed on iteration {i}"
            )
            fail_count += 1

    logger.info(
        f"'tt-smi -r' with ethernet training failed {fail_count}/{total_tries} times."
    )
    assert fail_count == 0, (
        "'tt-smi -r' with ethernet training failed a non-zero number of times."
    )


@pytest.mark.skipif(
    "os.getenv('BOARD') in ('bh-galaxy', 'bh-galaxy-revc', 'loudbox', 'quietbox2')",
    reason="Galaxy lacks a DMC; Loudbox/Quietbox2 have no STLink for dirty reset",
)
def test_dirty_reset():
    """
    Checks that the SMC comes up correctly after a "dirty" reset, where the
    DMC resets without the SMC requesting it. This is similar to the conditions
    that might be encountered after a NOC hang
    """
    test_name = "Dirty reset test"
    total_tries = min(MAX_TEST_ITERATIONS, 1000)
    fail_count = 0
    failure_fail_count = total_tries // 100

    for i in range(total_tries):
        if i % 10 == 0:
            logger.info(f"{test_name} iteration {i}/{total_tries}")

        result = dirty_reset_test()
        if not result:
            logger.warning(f"dirty reset failed on iteration {i}")
            fail_count += 1
        else:
            # Delay a moment before next run. Without this, tests seem to fail
            # TODO- would be best to determine why rapidly resetting like this
            # breaks enumeration.
            time.sleep(0.5)

    report_results(test_name, fail_count, total_tries)
    assert fail_count <= failure_fail_count, (
        f"{test_name} failed {fail_count}/{total_tries} times."
    )


@pytest.mark.skipif(
    "os.getenv('BOARD') in ('bh-galaxy', 'bh-galaxy-revc', 'loudbox', 'quietbox2')",
    reason="Galaxy lacks a DMC; Loudbox/Quietbox2 excluded from DMC-dependent stress tests",
)
def test_dmc_ping(arc_chip_dut, asic_id):
    """
    Repeatedly pings the DMC from the SMC to see what the average response time
    is. Ping statistics are printed to the log. These statistics are gathered
    without resetting the SMC. The `smi_reset` test will gather statistics
    for the SMC reset case.
    """
    arc_chip = pyluwen.detect_chips()[asic_id]
    total_tries = min(MAX_TEST_ITERATIONS, 1000)
    fail_count = 0
    dmfw_ping_avg = 0
    dmfw_ping_max = 0
    for i in range(total_tries):
        response = arc_chip.arc_msg(TT_SMC_MSG_PING_DM, True, False, 0, 0, 1000)
        if response[0] != 1 or response[1] != 0:
            logger.warning(f"Ping failed on iteration {i}")
            fail_count += 1
        duration = arc_chip.axi_read32(PING_DMFW_DURATION_REG_ADDR)
        dmfw_ping_avg += duration / total_tries
        dmfw_ping_max = max(dmfw_ping_max, duration)
    logger.info(
        f"Ping statistics: {total_tries - fail_count} successful pings, "
        f"{fail_count} failed pings."
    )
    # Recalculate the average ping time
    logger.info(
        f"Average DMFW ping time: {dmfw_ping_avg:.2f} ms, "
        f"Max DMFW ping time: {dmfw_ping_max:.2f} ms."
    )
    report_results("DMC ping test", fail_count, total_tries)
    assert fail_count == 0, "DMC ping test failed a non-zero number of times."


def test_upgrade_from_18x(
    tmp_path: Path, board_name, unlaunched_dut, arc_chip_dut, asic_id
):
    upgrade_from_version_test(
        arc_chip_dut,
        tmp_path,
        board_name,
        unlaunched_dut,
        asic_id,
        "18.10.0",
        (13 << 16),
        (19 << 16),
    )

    upgrade_from_version_test(
        arc_chip_dut,
        tmp_path,
        board_name,
        unlaunched_dut,
        asic_id,
        "18.11.0",
        (14 << 16),
        (20 << 16),
    )

    upgrade_from_version_test(
        arc_chip_dut,
        tmp_path,
        board_name,
        unlaunched_dut,
        asic_id,
        "18.12.0",
        (15 << 16),
        (21 << 16),
    )


def test_upgrade_from_19_00(
    arc_chip_dut, tmp_path: Path, board_name, unlaunched_dut, asic_id
):
    upgrade_from_version_test(
        arc_chip_dut,
        tmp_path,
        board_name,
        unlaunched_dut,
        asic_id,
        "19.0.0",
        (16 << 16),
        (22 << 16),
        replace_bootloader=True,
    )


def test_temperature_sensors(arc_chip_dut, asic_id):
    test_name = "Temperature sensor test"
    total_tries = min(MAX_TEST_ITERATIONS, 100)
    fail_count = 0

    for _ in range(total_tries):
        fail_count += temperature_sensors_test(arc_chip_dut, asic_id)

    report_results(test_name, fail_count, total_tries)
    assert fail_count == 0, f"{test_name} failed {fail_count} times."


def test_process_detectors(arc_chip_dut, asic_id):
    test_name = "Process detector test"
    total_tries = min(MAX_TEST_ITERATIONS, 50)
    fail_count = 0

    for _ in range(total_tries):
        fc = process_detectors_test(arc_chip_dut, asic_id)
        if fc > 0:
            logger.error(f"Failed in iteration {_}")
        fail_count += fc

    report_results(test_name, fail_count, total_tries)
    assert fail_count == 0, f"{test_name} failed {fail_count} times."


def test_voltage_monitors(arc_chip_dut, asic_id):
    test_name = "Voltage monitor test"
    total_tries = min(MAX_TEST_ITERATIONS, 100)
    fail_count = 0

    for _ in range(total_tries):
        fc = voltage_monitors_test(arc_chip_dut, asic_id)
        if fc > 0:
            logger.error(f"Failed in iteration {_}")
        fail_count += fc

    report_results(test_name, fail_count, total_tries)
    assert fail_count == 0, f"{test_name} failed {fail_count} times."


def test_pvt_comprehensive(arc_chip_dut, asic_id):
    test_name = "Comprehensive PVT test"
    total_tries = min(MAX_TEST_ITERATIONS, 20)
    fail_count = 0

    for _ in range(total_tries):
        fc = pvt_comprehensive_test(arc_chip_dut, asic_id)
        if fc > 0:
            logger.error(f"Failed in iteration {_}")
        fail_count += fc

    report_results(test_name, fail_count, total_tries)
    assert fail_count == 0, f"{test_name} failed {fail_count} times."


def test_power_state_toggle(arc_chip_dut, asic_id, board_name):
    test_name = "Power state toggle test"
    total_tries = min(MAX_TEST_ITERATIONS, 100)
    fail_count = 0

    for _ in range(total_tries):
        fc = power_state_toggle_test(arc_chip_dut, asic_id, board_name)
        if fc > 0:
            logger.error(f"Failed in iteration {_}")
        fail_count += fc

    report_results(test_name, fail_count, total_tries)
    assert fail_count == 0, f"{test_name} failed {fail_count} times."


@pytest.mark.skipif(
    "os.getenv('BOARD') in ('bh-galaxy', 'bh-galaxy-revc', 'loudbox', 'quietbox2')",
    reason="Burnin not stable on Galaxy, Loudbox, or Quietbox2",
)
def test_power_virus(arc_chip_dut, asic_id):
    """
    - Run the power virus TTX workload (tt-burnin) for 180 seconds
    - The expectations are:
    -       TMON temperatures can be fetched from the device successfully
    -       The tt-burnin command completes
    """
    arc_chip = pyluwen.detect_chips()[asic_id]

    def read_ts_once(chip, sensor_idx: int):
        # ARC handler expects sensor id; returns status in response[1]
        for i in range(NUM_TS):
            rsp = chip.arc_msg(TT_SMC_MSG_READ_TS, True, False, i, 0, 1000)
        # Best-effort logging; exact response layout is FW-defined
        logger.info(f"READ_TS idx={sensor_idx} rsp={rsp}")
        # If status is present as second field, ensure success
        if len(rsp) > 1:
            assert rsp[1] == 0, f"READ_TS status error for TS[{sensor_idx}]"
        return rsp

    # Sample TMON before PV
    for _ in range(20):
        for ts in range(0, 8):
            try:
                read_ts_once(arc_chip, ts)
            except Exception as e:
                logger.warning(f"READ_TS pre-PV failed for idx {ts}: {e}")
        time.sleep(0.1)

    # Run tt-burnin for 180s and read_ts at the same time
    logger.info("Starting tt-burnin process for power virus test")
    burnin_process = subprocess.Popen(
        ["tt-burnin", "--no-reset"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    duration = 180  # 180 seconds
    fail_count = 0

    try:
        end_time = time.time() + duration
        while time.time() < end_time:
            # Read temperature sensors during burnin
            for ts in range(NUM_TS):
                try:
                    read_ts_once(arc_chip, ts)
                except Exception as e:
                    logger.warning(f"READ_TS during PV failed for idx {ts}: {e}")
                    fail_count += 1

            # Check if burnin process is still running
            if burnin_process.poll() is not None:
                logger.warning("tt-burnin process terminated early")
                fail_count += 1
                break

            time.sleep(1.0)  # Sample every second during power virus

    except Exception as e:
        logger.warning(f"Power virus test failed: {e}")
        fail_count += 1

    finally:
        # Stop tt-burnin
        logger.info("Stopping tt-burnin process")
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

    logger.info(
        f"Power virus test completed with {fail_count} temperature read failures"
    )
    assert fail_count == 0, (
        f"Power virus test failed with {fail_count} temperature read failures"
    )


@pytest.mark.skipif(
    "os.getenv('BOARD') in ('bh-galaxy', 'bh-galaxy-revc', 'loudbox', 'quietbox2')",
    reason="Burnin not stable on Galaxy, Loudbox, or Quietbox2",
)
def test_tensix_reset_then_burnin(arc_chip_dut, asic_id):
    """
    Reset every non-harvested Tensix tile, then run tt-burnin and check
    it exits successfully.
    """
    arc_chip = pyluwen.detect_chips()[asic_id]

    TENSIX_NOC_Y = list(range(2, 12))
    enabled_cols = int(arc_chip.get_telemetry().tensix_enabled_col)
    valid_x = _logical_tensix_x_coords(enabled_cols)
    all_tiles = [(x, y) for x in valid_x for y in TENSIX_NOC_Y]

    # We want to test the tensix reset message on low power.
    # Set high power after the message to allow NOC read/write to function properly.
    logger.info(f"Resetting {len(all_tiles)} Tensix tiles")
    arc_chip.set_power_state("low")
    for noc_x, noc_y in all_tiles:
        response = arc_chip.arc_msg(
            TT_SMC_MSG_TOGGLE_SINGLE_TENSIX_RESET,
            arg0=noc_x | (noc_y << 8),
        )
        assert response[1] == 0, (
            f"Tensix ({noc_x}, {noc_y}) reset failed with {response[1]}"
        )
    arc_chip.set_power_state("high")
    logger.info("  Succeeded")

    duration = 180
    logger.info(f"Running tt-burnin for {duration}s")

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
    logger.info("  Succeeded")


# ---------------------------------------------------------------------------
# ccfgovr (configurable firmware-table override) stress tests.
#
# These exercise the bh-mod override mechanism with the slow paths (tt-flash,
# tt-smi resets, A/B bank alternation, extreme values, throttling). The quick
# set/reset smoke check lives in e2e_smoke.py. Helpers are shared from there.
# ---------------------------------------------------------------------------


@pytest.fixture
def ccfgovr_clean(asic_id):
    """
    Bracket a ccfgovr test with a clean override state: clear all persisted
    overrides (and re-enumerate the chip) both before and after the test, so it
    starts from firmware defaults and does not leak state to later tests. Yields
    the bh-mod path. Non-autouse, so only ccfgovr tests are affected.
    """
    bh_mod = _require_bh_mod()
    wait_arc_boot(asic_id)
    _reset_ccfgovr(bh_mod, asic_id)
    yield bh_mod
    _reset_ccfgovr(bh_mod, asic_id)


def test_ccfgovr_bh_mod(ccfgovr_clean, unlaunched_dut, asic_id):
    """
    Test bh-mod can persist overrides in SPI flash via the A/B ccfgovr mechanism,
    and that they are not overridden by tt-flash.

    Covers:
    - chip_limits.tdp_limit
    - the kernel-throttler-at-AICLK-floor config
      (feature_enable.kernel_throttler_at_floor_en and
      chip_limits.kernel_throttler_stop_nops_freq), exposed through
      TAG_KERNEL_THROTTLER telemetry:
        - bit 0: feature enabled
        - bits [31:16]: stop-NOPs frequency in MHz
    """
    bh_mod = ccfgovr_clean

    # The fixture has already cleared overrides, so this is the FW default.
    tdp_ref = _read_tdp_limit(asic_id)
    logger.info(f"Reference tdp_limit_max: {tdp_ref}")

    tdp_target = tdp_ref - 5
    TARGET_STOP_FREQ = 800  # MHz, within [AICLK_FMIN_MIN=200, AICLK_FMIN_MAX=1400]
    kt_expected = 1 | (TARGET_STOP_FREQ << 16)

    # Persist overrides for both the TDP limit and the kernel throttler config.
    result = _bh_mod_set(
        bh_mod,
        f"chip_limits.tdp_limit={tdp_target}",
        "feature_enable.kernel_throttler_at_floor_en=true",
        f"chip_limits.kernel_throttler_stop_nops_freq={TARGET_STOP_FREQ}",
    )
    assert result.returncode == 0, f"bh-mod set failed, rc={result.returncode}"

    # Verify the overrides were applied by the running firmware.
    chip = wait_arc_boot(asic_id)
    measured_tdp = chip.get_telemetry().tdp_limit_max
    assert measured_tdp == tdp_target, (
        f"expected tdp_limit_max={tdp_target}, got {measured_tdp}"
    )
    measured_kt = read_telem(chip, TAG_KERNEL_THROTTLER)
    logger.info(f"kernel_throttler after set: 0x{measured_kt:08x}")
    assert measured_kt == kt_expected, (
        f"kernel throttler config not applied: expected 0x{kt_expected:08x}, "
        f"got 0x{measured_kt:08x}"
    )

    # Re-flash the firmware bundle and confirm the overrides survive.
    unlaunched_dut.launch()
    del chip  # force re-detection after the flash and reboot
    chip = wait_arc_boot(asic_id, timeout=60)
    measured_tdp_after_flash = chip.get_telemetry().tdp_limit_max
    assert measured_tdp_after_flash == tdp_target, (
        f"ccfgovr did not survive tt-flash: "
        f"expected tdp_limit_max={tdp_target}, got {measured_tdp_after_flash}"
    )
    measured_kt_after_flash = read_telem(chip, TAG_KERNEL_THROTTLER)
    logger.info(f"kernel_throttler after re-flash: 0x{measured_kt_after_flash:08x}")
    assert measured_kt_after_flash == kt_expected, (
        f"ccfgovr did not survive tt-flash: expected 0x{kt_expected:08x}, "
        f"got 0x{measured_kt_after_flash:08x}"
    )


def test_ccfgovr_latest_set_wins(ccfgovr_clean, unlaunched_dut, asic_id):
    """
    Repeatedly persist a new TDP override and verify the most recent value always
    wins. This exercises the A/B bank alternation (each set lands in the other
    bank with an incremented sequence number) from the host's point of view.
    """
    bh_mod = ccfgovr_clean
    tdp_ref = _read_tdp_limit(asic_id)

    for target in (tdp_ref - 5, tdp_ref - 8, tdp_ref - 3):
        result = _bh_mod_set(bh_mod, f"chip_limits.tdp_limit={target}")
        assert result.returncode == 0, f"bh-mod set failed, rc={result.returncode}"

        measured = wait_arc_boot(asic_id).get_telemetry().tdp_limit_max
        assert measured == target, (
            f"latest override did not win: expected {target}, got {measured}"
        )
        logger.info(f"ccfgovr latest-wins iteration applied tdp_limit={target}")


def test_ccfgovr_persists_across_multiple_flashes(
    ccfgovr_clean, unlaunched_dut, asic_id
):
    """
    A persisted override must survive several consecutive tt-flash cycles, not
    just one.
    """
    bh_mod = ccfgovr_clean
    target = _read_tdp_limit(asic_id) - 7

    result = _bh_mod_set(bh_mod, f"chip_limits.tdp_limit={target}")
    assert result.returncode == 0, f"bh-mod set failed, rc={result.returncode}"

    for i in range(3):
        unlaunched_dut.launch()
        chip = wait_arc_boot(asic_id, timeout=60)
        measured = chip.get_telemetry().tdp_limit_max
        assert measured == target, (
            f"override did not survive flash {i}: expected {target}, got {measured}"
        )
        logger.info(f"ccfgovr survived tt-flash cycle {i} (tdp_limit={measured})")
        del chip


def test_ccfgovr_extreme_tdp_limit_is_safe(ccfgovr_clean, unlaunched_dut, asic_id):
    """
    A ccfgovr override bypasses the ARC-message range validation used by
    SET_TDP_LIMIT, so an absurd persisted TDP value must not brick the chip:
    the firmware must still boot and remain responsive after a reset. Runtime
    throttling clamps the effective limit downstream.
    """
    bh_mod = ccfgovr_clean

    result = _bh_mod_set(bh_mod, "chip_limits.tdp_limit=100000")
    assert result.returncode == 0, f"bh-mod set failed, rc={result.returncode}"

    subprocess.run(
        "tt-smi -r --eth_train_skip".split(), capture_output=True, check=False
    )
    chip = wait_arc_boot(asic_id)
    # Telemetry must still be readable: the extreme override did not brick FW.
    telemetry = chip.get_telemetry()
    assert telemetry is not None, "telemetry unavailable after extreme TDP override"
    logger.info(
        f"chip responsive after extreme TDP override; "
        f"tdp_limit_max telemetry={telemetry.tdp_limit_max}"
    )
    del chip


def test_ccfgovr_tdp_limit_throttles(ccfgovr_clean, unlaunched_dut, asic_id):
    """
    Functional check that a persisted TDP override actually reaches the power
    throttler, not just telemetry.

    The hard assertion is that the persisted limit is reflected in telemetry
    (i.e. ccfgovr -> fw_table -> the running throttler config). For the power
    measurement we compare against the *unconstrained* baseline rather than an
    absolute value: at the high power state with no compute workload the board
    sits at its high-power operating floor (well above any low ASIC TDP because
    of board overhead), so a low limit can only ever throttle *relative* to a
    high limit -- it cannot pull input power down to the ASIC TDP number.

    NOTE: a strict input-power-vs-limit check would require a compute workload to
    push the board above its floor; without one this asserts only that a lower
    limit does not draw more than a high limit, and logs the delta.
    """
    bh_mod = ccfgovr_clean
    HIGH_TDP = 300  # W, effectively unconstrained on supported boards
    LOW_TDP = 75  # W
    SETTLE_S = 1.0
    NOISE_W = 5  # tolerance for measurement noise between the two readings

    def _measure_high_power_input(limit):
        assert _bh_mod_set(bh_mod, f"chip_limits.tdp_limit={limit}").returncode == 0, (
            f"bh-mod set tdp_limit={limit} failed"
        )
        chip = wait_arc_boot(asic_id)
        assert chip.get_telemetry().tdp_limit_max == limit, (
            f"persisted TDP limit {limit}W not reflected in telemetry"
        )
        try:
            chip.set_power_state("high")
        except Exception:
            pytest.skip("driver does not support power state control")
        time.sleep(SETTLE_S)  # allow power to settle in the high state
        power = read_telem(chip, TAG_INPUT_POWER)
        del chip
        return power

    power_high = _measure_high_power_input(HIGH_TDP)
    logger.info(f"input power @ {HIGH_TDP}W TDP limit: {power_high}W")

    power_low = _measure_high_power_input(LOW_TDP)
    logger.info(f"input power @ {LOW_TDP}W TDP limit: {power_low}W")

    logger.info(f"TDP throttle delta (high - low): {power_high - power_low}W")
    # A lower persisted limit must throttle, i.e. never draw *more* than the
    # unconstrained baseline (within measurement noise).
    assert power_low <= power_high + NOISE_W, (
        f"lower TDP limit ({LOW_TDP}W) did not throttle: drew {power_low}W vs "
        f"{power_high}W at {HIGH_TDP}W limit"
    )


def test_ccfgovr_res_all(ccfgovr_clean, unlaunched_dut, asic_id):
    """
    `bh-mod res` (no args) clears every persisted override, returning the chip to
    its firmware-default configuration.
    """
    bh_mod = ccfgovr_clean

    # The fixture already cleared overrides, so this is the FW default.
    default_tdp = _read_tdp_limit(asic_id)
    logger.info(f"firmware-default tdp_limit_max: {default_tdp}")

    target = default_tdp - 5
    assert _bh_mod_set(bh_mod, f"chip_limits.tdp_limit={target}").returncode == 0, (
        "bh-mod set failed"
    )
    measured = wait_arc_boot(asic_id).get_telemetry().tdp_limit_max
    assert measured == target, (
        f"override not applied: expected {target}, got {measured}"
    )

    res = _bh_mod_res(bh_mod)
    assert res.returncode == 0, (
        f"bh-mod res (all) failed, rc={res.returncode}: "
        f"{res.stderr.decode(errors='replace')}"
    )

    measured = wait_arc_boot(asic_id).get_telemetry().tdp_limit_max
    assert measured == default_tdp, (
        f"res (all) did not restore default tdp_limit: "
        f"expected {default_tdp}, got {measured}"
    )


def test_ccfgovr_res_specific(ccfgovr_clean, unlaunched_dut, asic_id):
    """
    Ensure resetting a specific parameter does not reset other parameters.
    """
    bh_mod = ccfgovr_clean

    default_tdp = _read_tdp_limit(asic_id)

    TARGET_STOP_FREQ = 800  # MHz, within [AICLK_FMIN_MIN=200, AICLK_FMIN_MAX=1400]
    kt_expected = 1 | (TARGET_STOP_FREQ << 16)
    tdp_target = default_tdp - 5

    # Persist two independent overrides.
    assert (
        _bh_mod_set(
            bh_mod,
            f"chip_limits.tdp_limit={tdp_target}",
            "feature_enable.kernel_throttler_at_floor_en=true",
            f"chip_limits.kernel_throttler_stop_nops_freq={TARGET_STOP_FREQ}",
        ).returncode
        == 0
    ), "bh-mod set failed"
    wait_arc_boot(asic_id)

    # Reset only the TDP override.
    res = _bh_mod_res(bh_mod, "chip_limits.tdp_limit")
    assert res.returncode == 0, (
        f"bh-mod res <conf> failed, rc={res.returncode}: "
        f"{res.stderr.decode(errors='replace')}"
    )

    chip = wait_arc_boot(asic_id)
    measured_tdp = chip.get_telemetry().tdp_limit_max
    measured_kt = read_telem(chip, TAG_KERNEL_THROTTLER)
    del chip

    assert measured_tdp == default_tdp, (
        f"res <conf> should reset tdp_limit to default {default_tdp}, "
        f"got {measured_tdp}"
    )
    assert measured_kt == kt_expected, (
        f"res <conf> must not disturb other overrides: "
        f"expected kt 0x{kt_expected:08x}, got 0x{measured_kt:08x}"
    )
