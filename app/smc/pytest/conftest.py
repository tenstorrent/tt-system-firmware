# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import logging
from pathlib import Path
import pytest
import sys
import os

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


def pytest_exception_interact(node, call, report):
    if report.failed:
        asic_id = node.config.getoption("--asic-id")
        logger.error(f"Test failed, running recovery for ASIC ID {asic_id}")
        try:
            smc_test_recovery.recover_smc(asic_id)
        except Exception as e:
            logger.error(f"Recovery failed: {e}")
