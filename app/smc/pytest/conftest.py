# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import logging
import os
import sys
from pathlib import Path

import pytest

# The scripts directory holds the helpers imported below
sys.path.append(str(Path(__file__).resolve().parents[3] / "scripts"))

import ccfgovr_utils
import smc_test_recovery
from e2e_smoke import DeviceAdapter, _prepare_and_launch_dut, wait_arc_boot

logger = logging.getLogger(__name__)


def pytest_addoption(parser):
    parser.addoption(
        "--asic-id", action="store", default=0, help="Set ASIC IDX in use", type=int
    )
    parser.addoption(
        "--board",
        action="store",
        default=os.environ.get("BOARD"),
        help="Board name to use for testing",
    )
    parser.addoption(
        "--fwbundle",
        action="store",
        default="build/update.fwbundle",
        help="The FW bundle to use for flashing with no twister harness",
    )
    parser.addoption(
        "--skip-flash",
        action="store_true",
        default=False,
        help="Skip flashing the DUT; use firmware already running on the card",
    )


@pytest.fixture(scope="session")
def asic_id(request):
    return request.config.getoption("--asic-id")


@pytest.fixture(scope="session")
def board_name(request):
    return request.config.getoption("--board")


@pytest.fixture(scope="session")
def fwbundle(request):
    return request.config.getoption("--fwbundle")


@pytest.fixture(scope="session")
def skip_flash(request):
    return request.config.getoption("--skip-flash")


try:
    import twister_harness  # noqa: F401
except ImportError:

    @pytest.fixture(scope="session")
    def unlaunched_dut(fwbundle):
        return DeviceAdapter(fwbundle)


@pytest.fixture(scope="session")
def launched_arc_dut(unlaunched_dut, board_name, asic_id, skip_flash):
    _prepare_and_launch_dut(
        unlaunched_dut,
        flash_mcuboot_bl2=True,
        board_name=board_name,
        asic_id=asic_id,
        skip_flash=skip_flash,
    )
    return unlaunched_dut


def _leftover_ccfgovr(asic_id):
    try:
        return ccfgovr_utils.leftover_overrides(asic_id)
    except Exception as e:
        # The card runs older firmware without ccfgovr, or is in recovery
        logger.warning(f"Could not read ccfgovr overrides: {e}")
        return ""


@pytest.fixture(scope="session", autouse=True)
def teardown(asic_id, board_name):
    """
    ccfgovr overrides survive tt-flash and resets, so one left behind would
    change every later test on this card, including in later CI jobs. Clear
    any found at the start of the session, and fail the session if the
    tests leave one behind.
    """
    leftover = _leftover_ccfgovr(asic_id)
    if leftover:
        logger.warning(f"Clearing ccfgovr overrides left by an earlier run: {leftover}")
        if ccfgovr_utils.bh_mod_write(asic_id, board_name, "res", "--all") != 0:
            logger.error("Failed to clear leftover ccfgovr overrides")

    yield

    leftover = _leftover_ccfgovr(asic_id)
    if leftover:
        ccfgovr_utils.bh_mod_write(asic_id, board_name, "res", "--all")
        pytest.fail(f"Tests left ccfgovr overrides set on the card: {leftover}")


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


def pytest_exception_interact(node, call, report):
    if report.failed:
        asic_id = node.config.getoption("--asic-id")
        logger.error(f"Test failed, running recovery for ASIC ID {asic_id}")
        try:
            smc_test_recovery.recover_smc(asic_id)
        except Exception as e:
            logger.error(f"Recovery failed: {e}")
