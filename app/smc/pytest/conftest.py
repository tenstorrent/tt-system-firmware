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

try:
    from twister_harness import DeviceAdapter  # noqa: F401
except ImportError:
    # Twister's plugin normally provides this. Outside twister, use the
    # fallback adapter defined in e2e_smoke.
    @pytest.fixture(scope="session")
    def unlaunched_dut(fwbundle):
        from e2e_smoke import DeviceAdapter

        return DeviceAdapter(fwbundle)


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


def _write_sep(request, title):
    terminal = request.config.pluginmanager.get_plugin("terminalreporter")
    if terminal is not None:
        terminal.write_sep("=", title)


@pytest.fixture(scope="session")
def launched_arc_dut(request, unlaunched_dut, board_name, asic_id, skip_flash):
    """
    Launch the Zephyr DUT once per test session and return it.

    Tests that flash the DUT once and then run many cases share this fixture
    across every module in this directory.
    """
    # Imported lazily so collecting other modules in this directory does not
    # import the smoke suite.
    from e2e_smoke import _prepare_and_launch_dut

    # Session setup is reported under whichever test first requests it, so
    # mark it off from that test's own output.
    _write_sep(request, "DUT setup")
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
    _write_sep(request, "DUT setup done")
    return unlaunched_dut


@pytest.fixture()
def dal_chip(launched_arc_dut, asic_id):
    """
    Wait for SMC firmware to boot, then yield a tt-dal (session, tlb) for the
    chip, closed when the test ends
    """
    import dal_utils

    dal_utils.wait_arc_boot(asic_id)
    with dal_utils.open_chip(asic_id) as chip:
        yield chip


def pytest_exception_interact(node, call, report):
    if report.failed:
        asic_id = node.config.getoption("--asic-id")
        logger.error(f"Test failed, running recovery for ASIC ID {asic_id}")
        try:
            smc_test_recovery.recover_smc(asic_id)
        except Exception as e:
            logger.error(f"Recovery failed: {e}")
