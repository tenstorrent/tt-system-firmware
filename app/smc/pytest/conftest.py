# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import logging
from pathlib import Path
import pytest
import sys
import os
import time

# Add the scripts directory to sys.path to import recovery script
sys.path.append(str(Path(__file__).resolve().parents[3] / "scripts"))
import smc_test_recovery

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
    # twister_harness provides unlaunched_dut as a plugin fixture; defining it
    # here as well would shadow the plugin's.
    @pytest.fixture(scope="session")
    def unlaunched_dut(fwbundle):
        from e2e_smoke import DeviceAdapter

        return DeviceAdapter(fwbundle)


@pytest.fixture(scope="session")
def launched_arc_dut(unlaunched_dut, board_name, asic_id, skip_flash):
    """
    This fixture launches the Zephyr DUT once per test session, and returns
    a reference to the launched DUT. This is used by tests that need to
    flash the DUT once, and then run multiple tests against it.
    """
    from e2e_smoke import _prepare_and_launch_dut

    if skip_flash:
        logger.info("Skipping ARC DUT flash (--skip-flash)")
    else:
        logger.info("Flashing ARC DUT")
    _prepare_and_launch_dut(
        unlaunched_dut,
        flash_mcuboot_bl2=True,
        board_name=board_name,
        asic_id=asic_id,
        skip_flash=skip_flash,
    )
    time.sleep(1)  # Wait for ARC to start
    return unlaunched_dut


@pytest.fixture()
def arc_chip_dut(launched_arc_dut, asic_id):
    """
    This fixture returns the Zephyr DUT once the ARC chip is ready. Otherwise, it
    raises a runtime error.

    We use this approach rather than providing a reference to the pyluwen
    chip directly because tests may need to delete chip objects after they
    reset the ARC chip to induce pyluwen to close open file descriptors
    """
    from e2e_smoke import wait_arc_boot

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
