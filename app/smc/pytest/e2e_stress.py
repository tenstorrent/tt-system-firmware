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


logger = logging.getLogger(__name__)

SCRIPT_DIR = Path(os.path.dirname(os.path.abspath(__file__)))


def _skip_boards() -> bool:
    return os.getenv("BOARD") in ("galaxy", "loudbox", "quietbox2")


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
    "os.getenv('BOARD') in ('galaxy', 'loudbox', 'quietbox2')",
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
    "os.getenv('BOARD') in ('galaxy', 'loudbox', 'quietbox2')",
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
    "os.getenv('BOARD') in ('galaxy', 'loudbox', 'quietbox2')",
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
    "os.getenv('BOARD') in ('galaxy', 'loudbox', 'quietbox2')",
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


def test_upgrade_from_18x(tmp_path: Path, board_name, unlaunched_dut, arc_chip_dut):
    upgrade_from_version_test(
        arc_chip_dut,
        tmp_path,
        board_name,
        unlaunched_dut,
        "18.10.0",
        (13 << 16),
        (19 << 16),
    )

    upgrade_from_version_test(
        arc_chip_dut,
        tmp_path,
        board_name,
        unlaunched_dut,
        "18.11.0",
        (14 << 16),
        (20 << 16),
    )

    upgrade_from_version_test(
        arc_chip_dut,
        tmp_path,
        board_name,
        unlaunched_dut,
        "18.12.0",
        (15 << 16),
        (21 << 16),
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
    "os.getenv('BOARD') in ('galaxy', 'loudbox', 'quietbox2')",
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
    "os.getenv('BOARD') in ('galaxy', 'loudbox', 'quietbox2')",
    reason="Burnin not stable on Galaxy, Loudbox, or Quietbox2",
)
def test_tensix_reset_then_burnin(arc_chip_dut, asic_id):
    """
    Reset every non-harvested tensix tile, then run tt-burnin and check
    that it exits successfully.
    """
    arc_chip = pyluwen.detect_chips()[asic_id]

    TENSIX_NOC_Y = list(range(2, 12))
    enabled_cols = int(arc_chip.get_telemetry().tensix_enabled_col)
    valid_x = _logical_tensix_x_coords(enabled_cols)
    all_tiles = [(x, y) for x in valid_x for y in TENSIX_NOC_Y]

    # We want to test the tensix reset message on low power.
    # Set high power after the message to allow NOC read/write to function properly.
    arc_chip.set_power_state("low")
    for noc_x, noc_y in all_tiles:
        logger.info(f"Resetting ({noc_x},{noc_y})")
        response = arc_chip.arc_msg(
            TT_SMC_MSG_TOGGLE_SINGLE_TENSIX_RESET,
            arg0=noc_x | (noc_y << 8),
        )
        assert response[1] == 0, (
            f"Tensix ({noc_x}, {noc_y}) reset failed with {response[1]}"
        )
    arc_chip.set_power_state("high")

    logger.info(f"Reset {len(all_tiles)} tensix tiles, starting burnin")

    result = subprocess.run(
        ["tt-burnin", "--no-reset"],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=300,
    )

    logger.info(f"tt-burnin exited with code {result.returncode}")
    assert result.returncode == 0, "tt-burnin failed after tensix reset"
