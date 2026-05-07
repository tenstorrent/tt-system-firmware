#!/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import logging
import os
import subprocess
import re
import sys
import time
from pathlib import Path
from urllib.request import urlretrieve

import pyluwen
import pytest


def strip_ansi_codes(s):
    ansi_escape = re.compile(r"\x1B\[[0-?]*[ -/]*[@-~]")
    return ansi_escape.sub("", s)


class TwisterHarnessException(Exception):
    pass


class TwisterHarnessTimeoutException(Exception):
    pass


class DeviceConfig:
    def __init__(self, fwbundle=None):
        self.app_build_dir = Path(fwbundle).parent / "smc"


class DeviceAdapter:
    def __init__(self, fwbundle=None):
        self.fwbundle = fwbundle
        self.device_config = DeviceConfig(fwbundle)

    def launch(self):
        result = subprocess.run(
            ["tt-flash", "flash", str(self.fwbundle), "--force"],
            capture_output=True,
            text=True,
        )

        assert "FLASH SUCCESS" in strip_ansi_codes(result.stdout)


@pytest.fixture(scope="session")
def unlaunched_dut(fwbundle):
    return DeviceAdapter(fwbundle)


TTZP = Path(__file__).parents[3]
sys.path.append(str(TTZP / "scripts"))
from pcie_utils import rescan_pcie  # noqa: E402

logger = logging.getLogger(__name__)

SCRIPT_DIR = Path(os.path.dirname(os.path.abspath(__file__)))

# Constant memory addresses we can read from SMC
ARC_STATUS = 0x1FF30060

# ARC messages
TT_SMC_MSG_TEST = 0x90
MSG_TYPE_FORCE_AICLK = 0x33
MSG_TYPE_GET_AICLK = 0x34


@pytest.fixture(scope="session")
def launched_arc_dut(unlaunched_dut: DeviceAdapter):
    """
    This fixture launches the Zephyr DUT once per test session, and returns
    a reference to the launched DUT. This is used by tests that need to
    flash the DUT once, and then run multiple tests against it.
    """
    logger.info("Flashing ARC DUT")
    try:
        unlaunched_dut.launch()
    except TwisterHarnessException:
        pytest.exit("Failed to flash DUT")
    except TwisterHarnessTimeoutException:
        pytest.exit("DUT flash timed out")
    time.sleep(1)  # Wait for ARC to start
    return unlaunched_dut


def wait_arc_boot(asic_id, timeout=120):
    while True:
        try:
            chips = pyluwen.detect_chips()
            if len(chips) > asic_id:
                logger.info("Detected ARC chip")
                break
        except Exception:
            logger.warning("SMC firmware requires a reset. Rescanning PCIe bus")
        except BaseException as e:
            # We will continue through these exceptions, since pyluwen
            # sometimes throws rust exceptions when the chip is resetting.
            # log them with a higher severity so we can track them
            logger.error(f"Base exception error while detecting chips: {e}")
        time.sleep(0.5)
        rescan_pcie()
    chip = chips[asic_id]
    try:
        status = chip.axi_read32(ARC_STATUS)
    except Exception:
        logger.warning("SMC firmware requires a reset. Rescanning PCIe bus")
        rescan_pcie()
        status = chip.axi_read32(ARC_STATUS)
    assert (status & 0xFFFF0000) == 0xC0DE0000, "SMC firmware postcode is invalid"
    # Check post code status of firmware
    assert (status & 0xFFFF) >= 0x1D, "SMC firmware boot failed"
    # Remove references to chip objects so pyluwen will close file descriptors.
    # Otherwise these may become stale when SMC resets.
    return chips[asic_id]


@pytest.fixture()
def arc_chip_dut(launched_arc_dut, asic_id):
    """
    This fixture returns the Zephyr DUT once the ARC chip is ready. Otherwise, it
    raises a runtime error.

    We use this approach rather than providing a reference to the pyluwen
    chip directly because tests may need to delete chip objects after they
    reset the ARC chip to induce pyluwen to close open file descriptors
    """
    chip = wait_arc_boot(asic_id, timeout=15)
    del chip  # So we don't hold stale file descriptors
    return launched_arc_dut


def smi_reset_test(asic_id):
    """
    Helper to run tt-smi reset test. Returns True if test passed, False otherwise
    """
    smi_reset_cmd = "tt-smi -r --use_luwen"
    smi_reset_result = subprocess.run(
        smi_reset_cmd.split(), capture_output=True, check=False
    )
    if smi_reset_result.returncode != 0:
        logger.warning(f"'tt-smi -r' failed: {smi_reset_result.stdout.decode()}")

    return smi_reset_result.returncode == 0


def upgrade_from_version_test(
    arc_chip_dut,
    tmp_path: Path,
    board_name,
    unlaunched_dut,
    version,
    m3_version_base,
    cmfw_version_base,
    asic_id,
):
    # flash "base" firmware to update from
    # Also tests downgrade
    URL = f"https://github.com/tenstorrent/tt-firmware/releases/download/v{version}/fw_pack-{version}.fwbundle"
    targz = tmp_path / "fw_pack.tar.gz"

    urlretrieve(URL, targz)
    try:
        result = subprocess.run(
            ["tt-flash", "flash", "--fw-tar", str(targz), "--force"],
            capture_output=True,
            text=True,
        )

        assert "FLASH SUCCESS" in strip_ansi_codes(result.stdout)

    except subprocess.CalledProcessError as e:
        logger.error(
            f"tt-flash flash --fw_tar {targz} --force: failed with error: {e}\n{result.stdout}"
        )
        assert False

    time.sleep(0.5)

    arc_chip = pyluwen.detect_chips()[asic_id]

    fw_bundle_base = arc_chip.as_wh().get_telemetry().fw_bundle_version

    assert m3_version_base == arc_chip.as_wh().get_telemetry().m3_app_fw_version
    assert cmfw_version_base == arc_chip.as_wh().get_telemetry().arc0_fw_version
    assert (
        version
        == f"{((fw_bundle_base >> 24) & 0xFF)}.{((fw_bundle_base >> 16) & 0xFF)}.{(fw_bundle_base >> 8) & 0xFF}"
    )

    logger.info(
        f"Baseline FW: fwbundle:0x{fw_bundle_base:08x} m3:0x{m3_version_base:08x} cm:{cmfw_version_base:08x}"
    )

    # flash firmware to update to (the one build in repository)
    del arc_chip
    unlaunched_dut.launch()
    time.sleep(0.5)

    arc_chip = pyluwen.detect_chips()[asic_id]
    arc0_version_new = arc_chip.as_wh().get_telemetry().arc0_fw_version
    fwbundle_version_new = arc_chip.as_wh().get_telemetry().fw_bundle_version
    m3_version_new = arc_chip.as_wh().get_telemetry().m3_app_fw_version

    assert f"{((19 << 24) | (10 << 16) | (1)):08x}" == f"{fwbundle_version_new:08x}"
    assert f"{((2 << 24) | (43 << 16)):08x}" == f"{arc0_version_new:08x}"
    assert f"{(5 << 24 | 12 << 16):08x}" == f"{m3_version_new:08x}"

    logger.info(
        f"FW under test: fwbundle:0x{fwbundle_version_new:08x} m3:0x{m3_version_new:08x} cm:{arc0_version_new:08x}"
    )


def test_upgrade_from_19_0(
    arc_chip_dut,
    tmp_path: Path,
    board_name,
    unlaunched_dut,
    asic_id,
):
    upgrade_from_version_test(
        arc_chip_dut,
        tmp_path,
        board_name,
        unlaunched_dut,
        "19.0.0",
        (5 << 24) | (12 << 16),
        (2 << 24) | (36 << 16),
        asic_id,
    )


def test_arc_msg(arc_chip_dut, asic_id):
    """
    Runs a smoke test to verify that the ARC firmware can receive ARC messages
    """
    # Send a test message. We expect response to be incremented by 1
    arc_chip = pyluwen.detect_chips()[asic_id]

    status = arc_chip.axi_read32(ARC_STATUS)
    logger.info("ARC Status: %08x", status)

    response = arc_chip.arc_msg(TT_SMC_MSG_TEST, True, False, 20, 0, 1000)

    assert response[0] == 21, "SMC did not respond to test message"
    assert response[1] == 0, "SMC response invalid"
    logger.info('SMC ping message response "%d"', response[0])
    # Post code should have updated after first message
    status = arc_chip.axi_read32(ARC_STATUS)
    logger.info("ARC Status: %08x", status)


def test_fw_bundle_version(arc_chip_dut, asic_id):
    """
    Checks that the fw bundle version in telemetry matches the repo
    """
    arc_chip = pyluwen.detect_chips()[asic_id]
    telemetry = arc_chip.get_telemetry()

    exp_bundle_version = 19 << 24 | 10 << 16 | 1
    assert telemetry.fw_bundle_version == exp_bundle_version, (
        f"Firmware bundle version mismatch: {telemetry.fw_bundle_version:#010x} != {exp_bundle_version:#010x}"
    )
    logger.info(f"FW bundle version: {telemetry.fw_bundle_version:#010x}")


def test_boot_status(arc_chip_dut, asic_id):
    """
    Validates the boot status of the ARC firmware
    """
    # Read the boot status register and validate that it is correct
    arc_chip = pyluwen.detect_chips()[asic_id]
    status = arc_chip.axi_read32(ARC_STATUS)
    assert (status & 0xFFFF0000) == 0xC0DE0000, "SMC firmware postcode is invalid"
    logger.info(f"SMC boot status {status:08x}")


def test_smi_reset(arc_chip_dut, asic_id):
    """
    Checks that tt-smi resets are working successfully
    """
    total_tries = 1
    fail_count = 0
    for i in range(total_tries):
        logger.info(f"Iteration {i}:")
        result = smi_reset_test(asic_id)

        if not result:
            logger.warning(f"tt-smi reset failed on iteration {i}")
            fail_count += 1

    logger.info(f"'tt-smi -r' failed {fail_count}/{total_tries} times.")
    assert fail_count == 0, "'tt-smi -r' failed a non-zero number of times."


def test_aiclk(arc_chip_dut, asic_id):
    arc_chip = pyluwen.detect_chips()[asic_id]
    TARGET_AICLKS = [
        1000,
        800,
        250,
    ]

    for clk in TARGET_AICLKS:
        arc_chip.arc_msg(MSG_TYPE_FORCE_AICLK, True, False, clk, 0, 1000)
        # Delay to allow AICLK to settle
        time.sleep(0.1)
        aiclk = arc_chip.arc_msg(MSG_TYPE_GET_AICLK)[0]
        assert aiclk == clk, f"Failed to set clock to {clk} MHz"
        logger.info(f"AICLK set to {aiclk} MHz successfully")


def test_power_virus(arc_chip_dut, asic_id):
    """
    - Run the power virus TTX workload (tt-burnin) for 180 seconds
    - The expectations are:
    -       temperatures can be fetched from the device successfully
    -       The tt-burnin command completes
    """
    arc_chip = pyluwen.detect_chips()[asic_id]
    # Raise to high power state
    arc_chip.set_power_state("high")

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
            t = (arc_chip.get_telemetry().asic_temperature & 0xFFFF) / 16
            p = arc_chip.get_telemetry().tdp & 0xFFFF
            logger.info(f"Temperature: {t:4.1f}, Power {p}")
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
