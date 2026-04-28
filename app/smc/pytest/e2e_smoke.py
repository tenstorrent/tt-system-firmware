#!/bin/env python3

# Copyright (c) 2024 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import logging
import os
import shutil
import subprocess
import re
import sys
import time
from pathlib import Path
from urllib.request import urlretrieve

import pyluwen
import pytest
import get_ttzp_version


def strip_ansi_codes(s):
    ansi_escape = re.compile(r"\x1B\[[0-?]*[ -/]*[@-~]")
    return ansi_escape.sub("", s)


try:
    from twister_harness import DeviceAdapter
    from twister_harness.exceptions import (
        TwisterHarnessException,
        TwisterHarnessTimeoutException,
    )
except ImportError:
    # Fallback if twister_harness is not available
    print("Warning: twister_harness module not found. Using fallback harness")

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
                ["tt-flash", str(self.fwbundle), "--force"],
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
import smc_test_recovery  # noqa: E402
import dmc_reset  # noqa: E402

logger = logging.getLogger(__name__)

SCRIPT_DIR = Path(os.path.dirname(os.path.abspath(__file__)))


def _skip_boards(board_name) -> bool:
    return board_name in ("loudbox", "quietbox2") or "galaxy" in board_name.lower()


REFCLK_HZ = 50_000_000

# Constant memory addresses we can read from SMC
ARC_STATUS = 0x80030060
ARC_MISC_CTRL = 0x80030100
BOOT_STATUS = 0x80030408
ERROR_STATUS0 = 0x80030410
PCIE_INIT_CPL_TIME_REG_ADDR = 0x80030438
CMFW_START_TIME_REG_ADDR = 0x8003043C
ARC_START_TIME_REG_ADDR = 0x80030440
ARC_HANG_PC_REG_ADDR = 0x80030454
TELEMETRY_DATA_REG_ADDR = 0x80030430

# Functional efuse MMIO mapping (32-bit words)
EFUSE_DFT0_MEM_BASE_ADDR = 0x80040000
EFUSE_BOX_ADDR_ALIGN = 0x2000
EFUSE_BOX_FUNC = 2

FUSE_ASIC_ID_LOW_START_BIT = 1600
FUSE_ASIC_ID_HIGH_START_BIT = 5568

# ETH tile registers
# Using this as a scratch register to verify ETH tile reset
TRISC0_RESET_PC_ADDR = 0xFFB12228

# ARC messages
TT_SMC_MSG_REINIT_TENSIX = 0x20
TT_SMC_MSG_FORCE_AICLK = 0x33
TT_SMC_MSG_GET_AICLK = 0x34
TT_SMC_MSG_TEST = 0x90
TT_SMC_MSG_TOGGLE_TENSIX_RESET = 0xAF
TT_SMC_MSG_TOGGLE_SINGLE_TENSIX_RESET = 0xAE
TT_SMC_MSG_PING_DM = 0xC0
TT_SMC_MSG_SET_WDT = 0xC1
TT_SMC_MSG_READ_TS = 0x1B
TT_SMC_MSG_READ_PD = 0x1C
TT_SMC_MSG_READ_VM = 0x1D
TT_SMC_MSG_SET_TDP_LIMIT = 0x22
TT_SMC_MSG_SET_ASIC_HOST_FMAX = 0x23
TT_SMC_MSG_CHARACTERISATION = 0xC6
TT_SMC_MSG_COUNTER = 0x35
TT_SMC_MSG_TOGGLE_GDDR_RESET = 0xB6
TT_SMC_MSG_TOGGLE_ETH_RESET = 0xB0

# ETH toggle reset (eth_tile_reset_rqst) — response[1] uses eth_reset_err from ARC
ETH_RESET_ERR_INVALID_MASK = 1

# Characterization submessage IDs
TT_SUB_MSG_SET_HOST_REQUESTED_FMIN = 0x1
TT_SUB_MSG_SET_KERNEL_THROTTLER_ENABLED = 0x2
TT_SUB_MSG_SET_KERNEL_THROTTLER_STOP_NOPS_FREQ = 0x3

# Telemetry tags
TAG_TDP = 7
TAG_DM_APP_FW_VERSION = 26
TAG_ETH_LIVE_STATUS = 21
TAG_CM_FW_VERSION = 29
TAG_ENABLED_ETH = 35
TAG_INPUT_POWER = 54
TAG_ASIC_ID_HIGH = 61
TAG_ASIC_ID_LOW = 62
TAG_HOST_AICLK_LIMIT = 70
TAG_KERNEL_THROTTLER = 75

NUM_PD = 16
NUM_VM = 8
NUM_TS = 8

NUM_ETH = 14


def read_telem(arc_chip, telem_idx):
    table_addr = arc_chip.axi_read32(TELEMETRY_DATA_REG_ADDR)
    telem = arc_chip.axi_read32(table_addr + telem_idx * 4)

    return telem


def read_functional_efuse_word(arc_chip, word_idx):
    func_base = EFUSE_DFT0_MEM_BASE_ADDR + EFUSE_BOX_FUNC * EFUSE_BOX_ADDR_ALIGN
    return arc_chip.axi_read32(func_base + word_idx * 4)


def convert_telemetry_to_float(value):
    INT32_MIN = -2147483648

    if value == INT32_MIN:
        return sys.float_info.max
    else:
        return value / 65536.0


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


def wait_arc_boot(asic_id, timeout=15):
    start = time.time()
    # Attempt to detect the ARC chip for 15 seconds
    timeout = timeout
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
        if time.time() - start > timeout:
            # Dump the SMC state for debugging
            smc_test_recovery.recover_smc(asic_id)
            logger.error("Did not detect ARC chip within timeout period")
            pytest.exit("Did not detect ARC chip within timeout period")
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


def check_chip_count(board_name):
    chips = pyluwen.detect_chips()
    if "galaxy" in board_name:
        assert len(chips) == 32, f"Expected 32 BH chips on Galaxy, found {len(chips)}"
    elif "loudbox" in board_name:
        assert len(chips) == 8, f"Expected 8 BH chips on Loudbox, found {len(chips)}"
    elif "quietbox2" in board_name:
        assert len(chips) == 4, f"Expected 4 BH chips on Quietbox2, found {len(chips)}"
    elif "p300" in board_name:
        assert len(chips) == 2, f"Expected 2 BH chips on P300, found {len(chips)}"
    else:
        assert len(chips) == 1, f"Expected 1 BH chip, found {len(chips)}"
    del chips


def upgrade_from_version_test(
    arc_chip_dut,
    tmp_path: Path,
    board_name,
    unlaunched_dut,
    version,
    dmfw_version_base,
    cmfw_version_base,
    replace_bootloader=False,
):
    if board_name is None:
        pytest.skip("Upgrade test requires --board set, skipping upgrade test")

    RECOVERY_URL = "https://github.com/tenstorrent/tt-system-firmware/releases/download/v18.12.2/fw-pack-v18.12.2-recovery.tar.gz"

    tar_recovery = tmp_path / "recovery.tar.gz"
    urlretrieve(RECOVERY_URL, tar_recovery)
    recovery_py = str(
        Path(__file__).parents[3]
        / "scripts/tooling/blackhole_recovery/recover-blackhole.py"
    )
    # versions we want to check upgrading from

    # Galaxy has no recovery solution, but also lacks the DMC- so we don't
    # need to directly flash DMFW to test upgrades. Skip the same path on Loudbox.
    if not _skip_boards(board_name):
        # flash recovery first
        try:
            subprocess.check_call(
                [
                    "python3",
                    recovery_py,
                    tar_recovery,
                    board_name,
                    "--force",
                    "--no-prompt",
                ]
            )
        except subprocess.CalledProcessError as e:
            logger.error(f"blackhole_recovery.py failed with error: {e}")
            assert False

    # flash "base" firmware to update from
    URL = f"https://github.com/tenstorrent/tt-firmware/releases/download/v{version}/fw_pack-{version}.fwbundle"
    targz = tmp_path / "fw_pack.tar.gz"

    urlretrieve(URL, targz)
    try:
        result = subprocess.run(
            [
                "tt-flash",
                str(targz),
                "--force",
                "--allow-major-downgrades",
            ],
            capture_output=True,
            text=True,
        )
        logger.info(f"tt-flash output: {result.stdout}")

        assert "FLASH SUCCESS" in strip_ansi_codes(result.stdout)

    except subprocess.CalledProcessError as e:
        logger.error(
            f"tt-flash flash --fw_tar {targz} --force --allow-major-downgrades: failed with error: {e}\n{result.stdout}"
        )
        assert False

    if replace_bootloader and not _skip_boards(board_name):
        # Newer firmware requires us to replace the production bootloader on the
        # DMC with one that will accept a firmware signed using our temporary key
        try:
            subprocess.check_call(
                [
                    "west",
                    "flash",
                    "--no-rebuild",
                    "-d",
                    str(
                        unlaunched_dut.device_config.app_build_dir.parent
                        / "mcuboot-bl2"
                    ),
                ]
            )
        except subprocess.CalledProcessError as e:
            logger.error(f"west flash failed with error: {e}")
            assert False
        # Wait for the ARC chip to boot
        wait_arc_boot(0, timeout=20)

    time.sleep(0.5)
    arc_chip = pyluwen.detect_chips()[0]
    if not _skip_boards(board_name):
        assert dmfw_version_base == read_telem(arc_chip, TAG_DM_APP_FW_VERSION)
    assert cmfw_version_base == read_telem(arc_chip, TAG_CM_FW_VERSION)
    del arc_chip
    # Make sure we see all ASICs after flashing
    check_chip_count(board_name)

    # flash firmware to update to
    unlaunched_dut.launch()

    time.sleep(0.5)
    arc_chip = pyluwen.detect_chips()[0]
    if not _skip_boards(board_name):
        assert get_ttzp_version.get_ttzp_version_u32(
            TTZP / "app/dmc/VERSION"
        ) == read_telem(arc_chip, TAG_DM_APP_FW_VERSION)
    assert get_ttzp_version.get_ttzp_version_u32(
        TTZP / "app/smc/VERSION"
    ) == read_telem(arc_chip, TAG_CM_FW_VERSION)
    del arc_chip
    # Check chip count again
    check_chip_count(board_name)


def pvt_comprehensive_test(arc_chip_dut, asic_id):
    fail_count = 0
    arc_chip = pyluwen.detect_chips()[asic_id]
    # Set high power state
    arc_chip.set_power_state("high")
    test_sensors = [
        (TT_SMC_MSG_READ_TS, 0),
        (TT_SMC_MSG_READ_PD, 19),
        (TT_SMC_MSG_READ_VM, 0),
    ]

    for msg_type, sensor_param in test_sensors:
        response = arc_chip.arc_msg(msg_type, True, False, sensor_param, 0, 5000)

        if response[0] == 0 or response[1] != 0:
            fail_count += 1
            logger.error(
                f"Error {msg_type} response with sensor {sensor_param}: Expected !0,0 Actual: {response[0]},{response[1]}"
            )
    return fail_count


def voltage_monitors_test(arc_chip_dut, asic_id):
    arc_chip = pyluwen.detect_chips()[asic_id]
    fail_count = 0

    vmin = 0.65
    vmax = 0.95

    for sensor_id in range(NUM_VM):
        response = arc_chip.as_bh().arc_msg_buf(
            [TT_SMC_MSG_READ_VM, sensor_id, 0, 0, 0, 0, 0, 0]
        )

        voltage = response[2] / 1000

        if voltage < vmin or voltage > vmax or response[0] != 0:
            fail_count += 1
            logger.error(
                f"Expected voltage between {vmin}..{vmax}, received {voltage} with response {response[0]}"
            )

    return fail_count


def process_detectors_test(arc_chip_dut, asic_id):
    arc_chip = pyluwen.detect_chips()[asic_id]
    fail_count = 0
    fmin = 95
    fmax = 240

    # Set high power state
    arc_chip.set_power_state("high")

    delay_chains = [19, 20, 21]
    for delay_chain in delay_chains:
        for sensor_id in range(NUM_PD):
            response = arc_chip.as_bh().arc_msg_buf(
                [TT_SMC_MSG_READ_PD, delay_chain, sensor_id, 0, 0, 0, 0, 0]
            )

            freq = convert_telemetry_to_float(response[2])

            if freq < fmin or freq > fmax or response[0] != 0:
                fail_count += 1
                logger.error(
                    f"Error in pd message response. Expect frequency {freq} in ({fmin}..{fmax}) with response 0 == {response[0]}"
                )

    return fail_count


def temperature_sensors_test(arc_chip_dut, asic_id):
    arc_chip = pyluwen.detect_chips()[asic_id]
    fail_count = 0
    tmin = 20
    tmax = 70
    for sensor_id in range(NUM_TS):
        response = arc_chip.as_bh().arc_msg_buf(
            [TT_SMC_MSG_READ_TS, sensor_id, 0, 0, 0, 0, 0, 0]
        )

        temp = convert_telemetry_to_float(response[2])
        if temp < tmin or temp > tmax or response[0] != 0:
            fail_count += 1
            logger.error(
                f"Error in temp msg response. Expect temperature {temp} in ({tmin}..{tmax} with response 0 == {response[0]}"
            )

    return fail_count


@pytest.mark.skipif(
    "os.getenv('BOARD') == 'orion_slt'",
    reason="Orion SLT has no recovery/downgrade path for major version upgrade test",
)
def test_upgrade_from_19_00(arc_chip_dut, tmp_path: Path, board_name, unlaunched_dut):
    upgrade_from_version_test(
        arc_chip_dut,
        tmp_path,
        board_name,
        unlaunched_dut,
        "19.0.0",
        (16 << 16),
        (22 << 16),
        replace_bootloader=True,
    )


def test_arc_msg(arc_chip_dut, asic_id):
    """
    Runs a smoke test to verify that the ARC firmware can receive ARC messages
    """
    # Send a test message. We expect response to be incremented by 1
    arc_chip = pyluwen.detect_chips()[asic_id]
    response = arc_chip.arc_msg(TT_SMC_MSG_TEST, True, False, 20, 0, 1000)
    assert response[0] == 21, "SMC did not respond to test message"
    assert response[1] == 0, "SMC response invalid"
    logger.info('SMC ping message response "%d"', response[0])
    # Post code should have updated after first message
    status = arc_chip.axi_read32(ARC_STATUS)
    assert status == 0xC0DE003F, "SMC firmware has incorrect status"


def test_dmc_msg(arc_chip_dut, asic_id):
    """
    Validates the DMC firmware is alive and responding to pings
    """
    # Send an ARC message to ping the DMC, and validate that it is online
    arc_chip = pyluwen.detect_chips()[asic_id]
    response = arc_chip.arc_msg(TT_SMC_MSG_PING_DM, True, False, 0, 0, 1000)
    assert response[0] == 1, "DMC did not respond to ping from SMC"
    assert response[1] == 0, "SMC response invalid"
    logger.info('DMC ping message response "%d"', response[0])


def test_counter_msg(arc_chip_dut, asic_id):
    """
    Validates the counter ARC message by exercising GET, FREEZE and CLEAR
    sub-commands on each throttler arbiter and checking return code == 0.
    """
    COUNTER_CMD_GET = 0
    COUNTER_CMD_CLEAR = 1
    COUNTER_CMD_FREEZE = 2
    COUNTER_BANK_THROTTLERS = 0
    COUNTER_THROTTLER_MASK_ALL = 0x3FF
    THROTTLER_NUM_ARBITERS = 9

    arc_chip = pyluwen.detect_chips()[asic_id]

    for arb in range(THROTTLER_NUM_ARBITERS):
        response = arc_chip.as_bh().arc_msg_buf(
            [
                TT_SMC_MSG_COUNTER,
                COUNTER_CMD_GET | (COUNTER_BANK_THROTTLERS << 8) | (arb << 16),
                0,
                0,
                0,
                0,
                0,
                0,
            ]
        )
        assert response[0] == 0, f"Counter GET arb={arb} failed with rc={response[0]}"
        logger.info("Throttler counter[%d]: count=%d", arb, response[2])

    response = arc_chip.arc_msg(
        TT_SMC_MSG_COUNTER,
        arg0=COUNTER_CMD_FREEZE | (COUNTER_BANK_THROTTLERS << 8),
        arg1=COUNTER_THROTTLER_MASK_ALL,
    )
    assert response[0] == 0, f"Counter FREEZE failed with rc={response[0]}"

    response = arc_chip.arc_msg(
        TT_SMC_MSG_COUNTER,
        arg0=COUNTER_CMD_CLEAR | (COUNTER_BANK_THROTTLERS << 8),
        arg1=COUNTER_THROTTLER_MASK_ALL,
    )
    assert response[0] == 0, f"Counter CLEAR failed with rc={response[0]}"


def test_boot_status(arc_chip_dut, asic_id):
    """
    Validates the boot status of the ARC firmware
    """
    arc_chip = pyluwen.detect_chips()[asic_id]
    status = arc_chip.axi_read32(BOOT_STATUS)
    err = arc_chip.axi_read32(ERROR_STATUS0)

    assert (status >> 1) & 0x3 == 0x2, "SMC HW boot status is not valid"
    assert err == 0, "FW Init error"


@pytest.mark.skipif(
    "os.getenv('BOARD') == 'orion_slt'",
    reason="Orion SLT DMC does not set this register",
)
def test_smbus_status(arc_chip_dut, asic_id):
    """
    Validates that the SMBUS tests run from the DMC firmware passed
    """
    # We have limited visibility into the DMC firmware, so we read
    # the ARC scratch register the DMC should have set within SMBUS
    # tests to check that they passed
    arc_chip = pyluwen.detect_chips()[asic_id]
    ARC_SCRATCH_63 = 0x80030400 + (63 * 4)
    status = arc_chip.axi_read32(ARC_SCRATCH_63)
    assert status == 0xFEEDFACE, "SMC firmware did not pass SMBUS tests"
    logger.info('SMC SMBUS status: "0x%x"', status)


def test_flash_write(arc_chip_dut, asic_id):
    """
    Validates that flash read/write works via pyluwen,
    since this is the same interface used by tt-flash
    """
    SPI_RX_TRAIN_ADDR = 0x13FFC
    SPI_RX_TRAIN_DATA = 0xA5A55A5A
    SCRATCH_REGION = 0x2800000
    SCRATCH_REGION_SIZE = 0x10000
    WRITE_SIZE = 0x8000
    NUM_ITERATIONS = 5

    arc_chip = pyluwen.detect_chips()[asic_id]
    # Read the SPI RX training region and validate that it is correct
    data = bytes(4)
    check_data = SPI_RX_TRAIN_DATA.to_bytes(4, byteorder="little")
    arc_chip.as_bh().spi_read(SPI_RX_TRAIN_ADDR, data)
    assert data == check_data, "SMC SPI RX training region is not valid"
    logger.info(
        f"SPI RX training region: 0x{int.from_bytes(data, byteorder='little'):x}"
    )

    # To best simulate the write pattern of tt-flash, write large chunks
    for i in range(NUM_ITERATIONS):
        for addr in range(
            SCRATCH_REGION, SCRATCH_REGION + SCRATCH_REGION_SIZE, WRITE_SIZE
        ):
            data = bytes(os.urandom(WRITE_SIZE))
            check_data = bytes(WRITE_SIZE)
            arc_chip.as_bh().spi_write(addr, data)
            arc_chip.as_bh().spi_read(addr, check_data)

            assert data == check_data, "SMC SPI write failed"
            logger.info(f"Write to scratch region: 0x{addr:x} passed")
        logger.info("Flash test %d of %d passed", i + 1, NUM_ITERATIONS)


def get_int_version_from_file(filename) -> int:
    with open(filename, "r") as f:
        version_data = f.readlines()
    version_dict = {}
    for line in version_data:
        if line:
            # Split the line into key-value pairs
            key_value = line.split("=")
            key = key_value[0].strip()

            if len(key_value) == 2:
                key = key_value[0].strip()
                try:
                    value = int(key_value[1].strip(), 0)
                except ValueError:
                    # Some values are strings
                    value = key_value[1].strip()
                version_dict[key] = value
            else:
                version_dict[key] = None

    if version_dict["EXTRAVERSION"]:
        version_rc = int(re.sub(r"[^\d]", "", version_dict["EXTRAVERSION"]))
    else:
        # version_dict["EXTRAVERSION"] is None or an empty string
        version_rc = 0

    version_int = (
        version_dict["VERSION_MAJOR"] << 24
        | version_dict["VERSION_MINOR"] << 16
        | version_dict["PATCHLEVEL"] << 8
        | version_rc
    )
    return version_int


def arc_watchdog_test(asic_id):
    """
    Helper to run ARC watchdog test. Returns True on pass, or False on
    failure
    """
    arc_chip = pyluwen.detect_chips()[asic_id]
    wdt_timeout = 1000
    # Setup ARC watchdog for a 1000ms timeout
    arc_chip.arc_msg(TT_SMC_MSG_SET_WDT, True, False, wdt_timeout, 0, 1000)
    # Sleep 1500, make sure we can still ping arc
    time.sleep(1.5)
    response = arc_chip.arc_msg(TT_SMC_MSG_TEST, True, False, 1, 0, 1000)
    if response[0] != 2:
        logger.warning("SMC did not respond to test message")
        return False
    if response[1] != 0:
        logger.warning("SMC response invalid")
        return False
    # Clear the ARC hang register- this should be set during a watchdog reset
    arc_chip.axi_write32(ARC_HANG_PC_REG_ADDR, 0)
    # Halt the ARC cores.
    arc_chip.axi_write32(ARC_MISC_CTRL, 0xF0)
    # Make sure we can still detect ARC core after 500 ms
    time.sleep(0.5)
    try:
        arc_chip = pyluwen.detect_chips()[asic_id]
        hang_pc = arc_chip.axi_read32(ARC_HANG_PC_REG_ADDR)
        if hang_pc != 0:
            logger.error(
                "DMC reset too early (hang PC set), ARC should still be online"
            )
            return False
    except Exception:
        # Ideally we would catch a more specific exception here, but pyluwen
        # does not currently have specific exception types
        logger.error("DMC reset too early, ARC should still be online")
        return False
    # Delay 600 more ms. Make sure the ARC has been reset.
    time.sleep(0.6)
    # Delete arc chip object, to make sure pyluwen drops open file descriptors
    del arc_chip
    try:
        arc_chip = pyluwen.detect_chips()[asic_id]
        hang_pc = arc_chip.axi_read32(ARC_HANG_PC_REG_ADDR)
        # If the ARC chip was reset, the hang program counter should have been set
        if hang_pc == 0:
            logger.warning(
                "ARC did not reset, waiting 10 additional seconds to see if ARC core resets"
            )
            del arc_chip
            time.sleep(10)
            rescan_pcie()
            arc_chip = pyluwen.detect_chips()[asic_id]
            hang_pc = arc_chip.axi_read32(ARC_HANG_PC_REG_ADDR)
            del arc_chip
            if hang_pc == 0:
                logger.error("ARC core was not reset after ten seconds")
                return False
    except Exception:
        # We expect an exception here since the ARC should be resetting
        pass
    except BaseException as e:
        # We will continue through these exceptions, since pyluwen
        # sometimes throws rust exceptions when the chip is resetting.
        # log them with a higher severity so we can track them
        logger.error(f"Base exception error while detecting chips: {e}")
    # Delay a bit, then rescan PCIe
    time.sleep(1.0)
    # Rescan PCIe, and see if ARC chip has been reset
    rescan_pcie()
    arc_chip = pyluwen.detect_chips()[asic_id]
    hang_pc = arc_chip.axi_read32(ARC_HANG_PC_REG_ADDR)
    if hang_pc == 0:
        logger.error("ARC core was not reset, but PCIe device re-enumerated?")
        return False
    logger.info(f"ARC was reset, hang PC 0x{hang_pc:08X}")
    # Make sure ARC can still ping the DMC
    response = arc_chip.arc_msg(TT_SMC_MSG_PING_DM, True, False, 0, 0, 1000)
    if response[0] != 1:
        logger.warning("DMC did not respond to ping after reset")
        return False
    if response[1] != 0:
        logger.warning("SMC response invalid after reset")
        return False
    logger.info('DMC ping message response "%d"', response[0])
    return True


def test_arc_watchdog(arc_chip_dut, asic_id):
    """
    Validates that the DMC firmware watchdog for the ARC will correctly
    reset the chip
    """
    assert arc_watchdog_test(asic_id), "ARC watchdog test failed"


def pcie_fw_load_time_test(asic_id):
    """
    Helper to run PCIe FW load time test. Returns True if passed, False if failed
    """
    arc_chip = pyluwen.detect_chips()[asic_id]
    duration_deadline = 40  # 40 ms.
    pcie_init_cpl_time = arc_chip.axi_read32(PCIE_INIT_CPL_TIME_REG_ADDR)
    cmfw_start_time = arc_chip.axi_read32(CMFW_START_TIME_REG_ADDR)
    arc_start_time = arc_chip.axi_read32(ARC_START_TIME_REG_ADDR)
    # Wait up to 5 seconds for DMC to boot and sync with ARC
    timeout = time.time() + 5
    while arc_start_time == 0 and time.time() < timeout:
        # Wait for the DMC to report the ARC start time
        logger.info("Waiting for ARC start time to be set...")
        time.sleep(0.1)
        arc_start_time = arc_chip.axi_read32(ARC_START_TIME_REG_ADDR)

    if arc_start_time == 0:
        logger.warning("DMC never reported ARC start time")
        return False

    duration_in_ms = (pcie_init_cpl_time - arc_start_time) / REFCLK_HZ * 1000

    logger.info(
        f"BOOTROM start timestamp: {arc_start_time}, "
        f"CMFW start timestamp: {cmfw_start_time}, "
        f"PCIe init completion timestamp: {pcie_init_cpl_time}."
    )

    if duration_in_ms > duration_deadline:
        logger.warning(
            f"PCIe firmware load time exceeded {duration_deadline}ms: {duration_in_ms:.4f}ms"
        )
        return False

    return True


def test_pcie_fw_load_time(arc_chip_dut, asic_id):
    """
    Checks PCIe firmware load time is within 40ms.
    This test needs to be run after production reset.
    """
    assert pcie_fw_load_time_test(asic_id), "PCIe firmware load time test failed"


def test_fw_bundle_version(arc_chip_dut, asic_id):
    """
    Checks that the fw bundle version in telemetry matches the repo
    """
    arc_chip = pyluwen.detect_chips()[asic_id]
    telemetry = arc_chip.get_telemetry()

    exp_bundle_version = get_int_version_from_file(SCRIPT_DIR.parents[2] / "VERSION")
    assert telemetry.fw_bundle_version == exp_bundle_version, (
        f"Firmware bundle version mismatch: {telemetry.fw_bundle_version:#010x} != {exp_bundle_version:#010x}"
    )
    logger.info(f"FW bundle version: {telemetry.fw_bundle_version:#010x}")


def test_telemetry_asic_id_from_functional_efuse(arc_chip_dut, asic_id):
    """Validate ASIC ID telemetry tags are sourced from functional efuse words."""
    arc_chip = pyluwen.detect_chips()[asic_id]

    asic_id_low_word_idx = FUSE_ASIC_ID_LOW_START_BIT // 32
    asic_id_high_word_idx = FUSE_ASIC_ID_HIGH_START_BIT // 32

    expected_low = read_functional_efuse_word(arc_chip, asic_id_low_word_idx)
    expected_high = read_functional_efuse_word(arc_chip, asic_id_high_word_idx)

    telem_high = read_telem(arc_chip, TAG_ASIC_ID_HIGH)
    telem_low = read_telem(arc_chip, TAG_ASIC_ID_LOW)

    assert telem_high == expected_high, (
        f"TAG_ASIC_ID_HIGH mismatch: telemetry=0x{telem_high:08x}, "
        f"efuse=0x{expected_high:08x}"
    )
    assert telem_low == expected_low, (
        f"TAG_ASIC_ID_LOW mismatch: telemetry=0x{telem_low:08x}, "
        f"efuse=0x{expected_low:08x}"
    )


def _tt_smi_reset_base():
    if os.getenv("BOARD") == "orion_slt":
        return "tt-smi -r --use_luwen"
    return "tt-smi -r"


def smi_reset_test(asic_id):
    """
    Helper to run tt-smi reset test. Returns True if test passed, False otherwise
    """
    smi_reset_cmd = f"{_tt_smi_reset_base()} --eth_train_skip"
    smi_reset_result = subprocess.run(
        smi_reset_cmd.split(), capture_output=True, check=False
    )
    if smi_reset_result.returncode != 0:
        logger.warning(f"'tt-smi -r' failed: {smi_reset_result.stdout.decode()}")
        smc_test_recovery.recover_smc(asic_id)

    return smi_reset_result.returncode == 0


def smi_reset_with_eth(asic_id):
    """
    Helper to run tt-smi reset test with ethernet training. Returns True if test passed, False otherwise
    """
    smi_reset_cmd = _tt_smi_reset_base()
    smi_reset_result = subprocess.run(
        smi_reset_cmd.split(), capture_output=True, check=False
    )
    if smi_reset_result.returncode != 0:
        logger.warning(
            f"'tt-smi -r' with ethernet training failed: {smi_reset_result.stdout.decode()}"
        )
        smc_test_recovery.recover_smc(asic_id)

    return smi_reset_result.returncode == 0


def test_smi_reset(arc_chip_dut, asic_id):
    """
    Checks that tt-smi resets are working successfully
    """
    total_tries = 10
    fail_count = 0
    for i in range(total_tries):
        logger.info(f"Iteration {i}:")
        result = smi_reset_test(asic_id)

        if not result:
            logger.warning(f"tt-smi reset failed on iteration {i}")
            fail_count += 1

    logger.info(f"'tt-smi -r' failed {fail_count}/{total_tries} times.")
    assert fail_count == 0, "'tt-smi -r' failed a non-zero number of times."


def test_smi_reset_with_eth(arc_chip_dut, asic_id):
    """
    Checks that tt-smi resets with ethernet training are working successfully
    """
    # Ethernet training takes significantly longer, so we reduce the number of
    # iterations to keep test runtime reasonable, while still providing some confidence in stability
    total_tries = 2
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


def dirty_reset_test():
    """
    Helper to execute dirty reset test. Returns True if test passes, False otherwise
    """
    timeout = 60  # seconds to wait for SMC boot

    # Use dmc-reset script as a library to reset the DMC
    def args():
        return None

    args.config = dmc_reset.DEFAULT_DMC_CFG
    args.debug = 0
    args.openocd = dmc_reset.DEFAULT_OPENOCD
    args.scripts = dmc_reset.DEFAULT_SCRIPTS_DIR
    args.jtag_id = None
    args.hexfile = None

    ret = dmc_reset.reset_dmc(args)
    if ret != os.EX_OK:
        logger.warning("DMC reset failed on iteration")
        return False
    ret = dmc_reset.wait_for_smc_boot(timeout)
    if ret != os.EX_OK:
        logger.warning("SMC did not boot after dirty reset")
        return False
    return True


@pytest.mark.skipif(
    "os.getenv('BOARD') in ('bh-galaxy', 'loudbox', 'quietbox2')",
    reason="Galaxy: no DMC path; Loudbox/Quietbox2: no STLink for OpenOCD dirty reset",
)
def test_dirty_reset():
    """
    Checks that the SMC comes up correctly after a "dirty" reset, where the
    DMC resets without the SMC requesting it. This is similar to the conditions
    that might be encountered after a NOC hang
    """
    total_tries = 10
    fail_count = 0

    for i in range(total_tries):
        logger.info(f"Iteration {i}:")
        result = dirty_reset_test()
        if not result:
            logger.warning(f"dirty reset failed on iteration {i}")
            fail_count += 1
        else:
            logger.info(f"dirty reset passed on iteration {i}")

    logger.info(f"dirty reset failed {fail_count}/{total_tries} times.")
    assert fail_count == 0, "dirty reset failed a non-zero number of times."


def tensix_reset_sequence(arc_chip):
    """
    Careful sequence to reset all the Tensixes
    """
    TENSIX_RISC_RESET_ADDR = [0x80030040 + i * 4 for i in range(8)]
    SOFT_RESET_ADDR = 0xFFB121B0
    SOFT_RESET_DATA = 0x47800

    # Force AICLK to safe frequency
    arc_chip.arc_msg(TT_SMC_MSG_FORCE_AICLK, arg0=250, arg1=0)
    # Clear RISC reset registers
    for addr in TENSIX_RISC_RESET_ADDR:
        arc_chip.axi_write32(addr, 0)

    # The tensix reset message
    response = arc_chip.arc_msg(TT_SMC_MSG_TOGGLE_TENSIX_RESET)
    assert response[1] == 0, "SMC response invalid to toggle tensix reset message"
    response = arc_chip.arc_msg(TT_SMC_MSG_REINIT_TENSIX)
    assert response[1] == 0, "SMC response invalid to reinit tensix message"

    # Set soft reset registers inside Tensix
    arc_chip.noc_broadcast32(1, SOFT_RESET_ADDR, SOFT_RESET_DATA)

    # Release RISC reset registers
    for addr in TENSIX_RISC_RESET_ADDR:
        arc_chip.axi_write32(addr, 0xFFFFFFFF)

    # Unforce AICLK
    arc_chip.arc_msg(TT_SMC_MSG_FORCE_AICLK, arg0=0, arg1=0)


def test_tensix_reset(arc_chip_dut, asic_id):
    """
    Validates the Tensix reset sequence
    """
    # Unused register in Tensix. Use bit 0 as a scratch bit
    # "PREFECTH" is a typo carried over from the register name in the RTL
    arc_chip = pyluwen.detect_chips()[asic_id]

    try:
        arc_chip.set_power_state("high")
    except Exception:
        logger.info("No driver support for power IOCTL; continue.")

    ETH_RISC_PREFECTH_CTRL_ADDR = 0xFFB120B8
    # This Tensix coordinate (1-2) should be available in all current harvesting configs
    scratch_set = arc_chip.noc_read32(
        noc_id=0, x=1, y=2, addr=ETH_RISC_PREFECTH_CTRL_ADDR
    )
    scratch_set |= 1  # Set bit 0 as a scratch flag

    for i in range(10):
        logger.info(f"Starting Tensix reset test iteration {i}")
        # Set scratch bit before reset
        arc_chip.noc_write32(
            noc_id=0, x=1, y=2, addr=ETH_RISC_PREFECTH_CTRL_ADDR, data=scratch_set
        )

        tensix_reset_sequence(arc_chip)

        # Check that the Tensix reset worked by checking the scratch bit is cleared
        scratch_get = arc_chip.noc_read32(
            noc_id=0, x=1, y=2, addr=ETH_RISC_PREFECTH_CTRL_ADDR
        )
        assert scratch_get & 1 == 0, (
            "Tensix scratch bit not cleared. Tensix reset failed"
        )

        logger.info(f"Tensix reset test iteration {i} passed")


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


def test_tensix_reset_single(arc_chip_dut, asic_id):
    """
    Reset every non-harvested tensix one at a time and verify that only the
    target tile is affected.  For each tile the test:

    1. Writes a sentinel pattern to every enabled tile.
    2. Resets one tile and checks its register is cleared.
    3. Reads every *other* tile and checks the pattern is still intact.
    """
    arc_chip = pyluwen.detect_chips()[asic_id]

    TENSIX_NOC_Y = list(range(2, 12))
    TRISC0_RESET_PC = 0xFFB12228
    PATTERN = 0xA5A5

    enabled_cols = int(arc_chip.get_telemetry().tensix_enabled_col)
    assert enabled_cols != 0, "tensix_enabled_col is zero"

    valid_x = _logical_tensix_x_coords(enabled_cols)
    all_tiles = [(x, y) for x in valid_x for y in TENSIX_NOC_Y]

    arc_chip.set_power_state("high")

    # Write pattern to every tile once up front
    for x, y in all_tiles:
        arc_chip.noc_write32(noc_id=0, x=x, y=y, addr=TRISC0_RESET_PC, data=PATTERN)

    failed = 0

    for target_x, target_y in all_tiles:
        # Reset the target tile
        response = arc_chip.arc_msg(
            TT_SMC_MSG_TOGGLE_SINGLE_TENSIX_RESET,
            arg0=target_x | (target_y << 8),
        )
        if response[1] != 0:
            failed += 1
            arc_chip.noc_write32(
                noc_id=0, x=target_x, y=target_y, addr=TRISC0_RESET_PC, data=PATTERN
            )
            continue

        # Verify the target tile was cleared
        val = arc_chip.noc_read32(
            noc_id=0, x=target_x, y=target_y, addr=TRISC0_RESET_PC
        )
        if val != 0:
            failed += 1
            arc_chip.noc_write32(
                noc_id=0, x=target_x, y=target_y, addr=TRISC0_RESET_PC, data=PATTERN
            )
            continue

        # Verify no other tile was affected
        for x, y in all_tiles:
            if (x, y) == (target_x, target_y):
                continue
            val = arc_chip.noc_read32(noc_id=0, x=x, y=y, addr=TRISC0_RESET_PC)
            if val != PATTERN:
                failed += 1
                break

        # Restore pattern on the reset tile before moving to the next
        arc_chip.noc_write32(
            noc_id=0, x=target_x, y=target_y, addr=TRISC0_RESET_PC, data=PATTERN
        )

    assert failed == 0, f"{failed}/{len(all_tiles)} tiles failed"


def test_aiclk(arc_chip_dut, asic_id):
    arc_chip = pyluwen.detect_chips()[asic_id]
    TARGET_AICLKS = [
        1350,  # Max value we rise to with GO_BUSY
        800,  # Value we settle to with GO_IDLE_LONG
        200,  # Minimum AICLK
    ]

    for clk in TARGET_AICLKS:
        arc_chip.arc_msg(TT_SMC_MSG_FORCE_AICLK, True, False, clk, 0, 1000)
        # Delay to allow AICLK to settle
        time.sleep(0.1)
        aiclk = arc_chip.arc_msg(TT_SMC_MSG_GET_AICLK)[0]
        assert aiclk == clk, f"Failed to set clock to {clk} MHz"
        logger.info(f"AICLK set to {aiclk} MHz successfully")


def test_mcuboot(unlaunched_dut, asic_id):
    """
    Validates that the SMC falls back to the recovery image
    when the main image is not valid.
    """
    arc_chip = wait_arc_boot(asic_id, timeout=15)
    MCUBOOT_HEADER_ADDR = 0x29E000
    MCUBOOT_MAGIC = 0x96F3B83D
    # First, validate we are running the base image. A good way to check this is
    # to see that telemetry data is available
    try:
        arc_chip.get_telemetry()
    except Exception as e:
        assert False, f"Failed to get telemetry data: {e}"
    # Check that the MCUBOOT header magic is present
    buf = bytes(4)
    arc_chip.as_bh().spi_read(MCUBOOT_HEADER_ADDR, buf)
    magic = int.from_bytes(buf, "little")
    assert magic == MCUBOOT_MAGIC, (
        f"MCUBOOT magic not found at {MCUBOOT_HEADER_ADDR:#010x}"
    )
    logger.info(f"MCUBOOT magic found in main image header: 0x{magic:#010x}")
    # Now, erase the header of the main image, so that the SMC will fall
    # back to the recovery image
    logger.info("Erasing main image header to trigger recovery fallback")
    buf = bytes([0xFF] * 0x1000)
    arc_chip.as_bh().spi_write(MCUBOOT_HEADER_ADDR, buf)
    # Reset the SMC to trigger the fallback
    del arc_chip  # Force re-detection of the chip
    smi_reset_cmd = _tt_smi_reset_base()
    # tt-smi will fail here since it checks for valid telemetry after reset,
    # we still need to run it to trigger the SMC reboot
    subprocess.run(smi_reset_cmd.split(), capture_output=False, check=False)
    arc_chip = wait_arc_boot(asic_id, timeout=15)
    # Validate that the SMC has booted into the recovery image
    with pytest.raises(Exception):
        arc_chip.get_telemetry()
    logger.info("SMC telemetry data not available, as expected in recovery mode")
    # Readback the ROM header. We will need it present to use tt-flash
    header = bytes(0x1000)
    arc_chip.as_bh().spi_read(0x0, header)
    # Erase the ROM header to make sure we can boot recovery from the failover
    # descriptor
    arc_chip.as_bh().spi_write(0x0, buf)
    logger.info("Erased ROM header to force failover boot from recovery image")
    # Reset the SMC to trigger the fallback. Note that we cannot check
    # the return code here since tt-smi will fail due to missing telemetry.
    subprocess.run(smi_reset_cmd.split(), capture_output=False, check=False)
    arc_chip = wait_arc_boot(asic_id, timeout=15)
    with pytest.raises(Exception):
        arc_chip.get_telemetry()
    logger.info(
        "SMC telemetry data not available, as expected in "
        "recovery mode booted from failover"
    )
    # Now, restore the ROM header so we can flash a good image
    arc_chip.as_bh().spi_write(0x0, header)
    # Now, make sure we can flash a good image from recovery mode
    unlaunched_dut.launch()
    del arc_chip  # Force re-detection of the chip
    arc_chip = wait_arc_boot(asic_id, timeout=60)
    # Make sure we can get telemetry data again
    try:
        arc_chip.get_telemetry()
    except Exception as e:
        assert False, f"Failed to get telemetry data after recovery: {e}"
    # Check that the MCUBOOT header magic is present again
    buf = bytes(4)
    arc_chip.as_bh().spi_read(MCUBOOT_HEADER_ADDR, buf)
    magic = int.from_bytes(buf, "little")


def test_temperature_sensors(arc_chip_dut, asic_id):
    """
    Validates that the temperature sensor messages work and relay responses within reasonable bounds

    The message tested is READ_TS.
    The expectation is that the temperature returned is between 40 and 70
    """
    assert 0 == temperature_sensors_test(arc_chip_dut, asic_id)


def test_process_detectors(arc_chip_dut, asic_id):
    """
    Validates that the voltage monitor messages work and relay responses within reasonable bounds

    The message tested is READ_PD.
    The expectation is that the frequency returned is between 100 and 240
    """
    assert 0 == process_detectors_test(arc_chip_dut, asic_id), "test_pvt_msgs failed"


def test_voltage_monitors(arc_chip_dut, asic_id):
    """
    Validates that the voltage monitor messages work and relay responses within reasonable bounds

    The message tested is READ_VM.
    The expectation is that the voltage returned is between 0 and 1.
    """
    assert 0 == voltage_monitors_test(arc_chip_dut, asic_id), "test_pvt_msgs failed"


def test_pvt_comprehensive(arc_chip_dut, asic_id):
    """
    Validates that the PVT messages work and relay responses within reasonable bounds

    The messages tested are READ_TS, READ_PD, and READ_VM.
    The expectation is that the SMC response to these messages is 0.
    """
    assert 0 == pvt_comprehensive_test(arc_chip_dut, asic_id), "test_pvt_msgs failed"


def power_state_toggle_test(arc_chip_dut, asic_id, board_name):
    """
    Test toggling between high and low power states and verify TDP delta.

    Toggles between high and low power states and verifies that the TDP
    difference between the two states is greater than 80W.
    For galaxy, loudbox, and quietbox2, only tests power state setting without TDP validation.
    """
    expected_power_delta = 80
    settling_time = 0.5
    arc_chip = pyluwen.detect_chips()[asic_id]
    skip_tdp = _skip_boards(board_name)

    try:
        logger.info("Setting power state to high")
        arc_chip.set_power_state("high")
    except Exception as e:
        logger.info(f"No driver support for power state IOCTL: {e}")
        pytest.skip("Driver does not support power state control")

    time.sleep(settling_time)  # Allow power state to stabilize

    # Measure input power in high power state
    if not skip_tdp:
        high_power = read_telem(arc_chip, TAG_INPUT_POWER)
        logger.info(f"High power state input power: {high_power}W")

    logger.info("Setting power state to low")
    arc_chip.set_power_state("low")
    time.sleep(settling_time)  # Allow power state to stabilize

    # Measure input power in low power state
    if not skip_tdp:
        low_power = read_telem(arc_chip, TAG_INPUT_POWER)
        logger.info(f"Low power state input power: {low_power}W")

        power_delta = high_power - low_power
        logger.info(f"Power delta: {power_delta}W")

        # Verify delta is greater than expected
        assert power_delta > expected_power_delta, (
            f"Power delta ({power_delta}W) is not greater than {expected_power_delta}W"
        )

    return 0


@pytest.mark.skipif(
    "os.getenv('BOARD') == 'orion_slt'",
    reason="Orion SLT does not support the power state toggle flow",
)
def test_power_state_toggle(arc_chip_dut, asic_id, board_name):
    """
    Validates that toggling between high and low power states results in a TDP delta > 90W
    For galaxy, loudbox, and quietbox2, only validates power state setting functionality.
    """
    assert 0 == power_state_toggle_test(arc_chip_dut, asic_id, board_name), (
        "power_state_toggle_test failed"
    )


def test_eth_live_status(arc_chip_dut, asic_id):
    """
    Validates that the Ethernet live status reflects correct heartbeat status.

    Polls TAG_ETH_LIVE_STATUS every second until the heartbeat bitmask (lower 16
    bits) matches TAG_ENABLED_ETH. Fails if the condition is not met within 75 s.
    """
    # ETH FW can take up to 60 seconds to start its heartbeat.
    # How long we actually have to wait depends on the board type and
    # how long it has been since the ASIC was reset.
    # Running this test before eth_toggle_reset_* tests reduces the amount of waiting time.
    TIMEOUT_S = 75.0
    POLL_INTERVAL_S = 1.0

    arc_chip = pyluwen.detect_chips()[asic_id]
    eth_enabled = read_telem(arc_chip, TAG_ENABLED_ETH)
    start = time.monotonic()

    while True:
        elapsed = time.monotonic() - start
        eth_live_status = read_telem(arc_chip, TAG_ETH_LIVE_STATUS)
        heartbeat_status = eth_live_status & 0xFFFF

        logger.debug(
            f"[{elapsed:.1f}s] ETH enabled: {eth_enabled:#06x}, "
            f"heartbeat status: {heartbeat_status:#06x}"
        )

        if heartbeat_status == eth_enabled:
            logger.info(
                f"Heartbeat matched eth_enabled after {elapsed:.1f}s "
                f"({heartbeat_status:#06x})"
            )
            return

        if elapsed >= TIMEOUT_S:
            pytest.fail(
                f"Heartbeat status {heartbeat_status:#06x} did not match "
                f"eth_enabled {eth_enabled:#06x} within {TIMEOUT_S:.0f}s"
            )

        time.sleep(POLL_INTERVAL_S)


def send_eth_toggle_reset(arc_chip, eth_inst_mask, no_fw_reload=False, timeout=None):
    """Send TT_SMC_MSG_TOGGLE_ETH_RESET (eth_tile_reset_rqst).

    eth_inst_mask: bit N selects ETH instance N.
    no_fw_reload: when True, sets data[2] bit 0 to skip SPI FW reload and ReleaseEthReset.
    """
    msg = [
        TT_SMC_MSG_TOGGLE_ETH_RESET,
        eth_inst_mask,
        1 if no_fw_reload else 0,
        0,
        0,
        0,
        0,
        0,
    ]
    if timeout is not None:
        return arc_chip.as_bh().arc_msg_buf(msg, timeout=timeout)
    return arc_chip.as_bh().arc_msg_buf(msg)


def eth_id_to_noc0_coords(eth_inst) -> tuple[int, int]:
    PHYS_X_TO_NOC0 = [0, 1, 16, 2, 15, 3, 14, 4, 13, 5, 12, 6, 11, 7, 10, 8, 9]

    x = PHYS_X_TO_NOC0[eth_inst + 1]
    y = 1  # All ETH tiles are on the same row

    return x, y


def set_eth_scratch_pre_reset(arc_chip, eth_inst_mask):
    """
    Write a sentinel value to the scratch register for the given ETH instances.
    This is used to verify that the ETH tile was reset.
    Avoid writing to harvested ETH tiles, since that will hang the NOC.
    """
    for eth_inst in range(NUM_ETH):
        if not (1 << eth_inst) & eth_inst_mask:
            continue

        x, y = eth_id_to_noc0_coords(eth_inst)
        arc_chip.noc_write32(
            noc_id=0, x=x, y=y, addr=TRISC0_RESET_PC_ADDR, data=0xA5A5A5A5
        )


def check_eth_scratch_post_reset(arc_chip, eth_inst_mask):
    """
    Verify that the scratch register for the given ETH instances was cleared after reset.
    Avoid reading from harvested ETH tiles, since that will hang the NOC.
    """
    for eth_inst in range(NUM_ETH):
        if not (1 << eth_inst) & eth_inst_mask:
            continue

        x, y = eth_id_to_noc0_coords(eth_inst)
        scratch = arc_chip.noc_read32(noc_id=0, x=x, y=y, addr=TRISC0_RESET_PC_ADDR)
        assert scratch == 0, f"ETH {eth_inst} scratch register not cleared after reset"


def test_eth_toggle_reset_invalid_mask(arc_chip_dut, asic_id):
    """Reject ETH reset bitmask with bits outside the supported instance range."""
    arc_chip = pyluwen.detect_chips()[asic_id]
    bad_mask = 1 << 15
    response = send_eth_toggle_reset(arc_chip, bad_mask)
    assert response[0] != 0, "expected non-zero exit for invalid ETH instance mask"
    assert response[1] == ETH_RESET_ERR_INVALID_MASK, (
        f"expected ETH_RESET_ERR_INVALID_MASK ({ETH_RESET_ERR_INVALID_MASK}), got {response[1]}"
    )


def test_eth_toggle_reset_noop_mask(arc_chip_dut, asic_id):
    """Zero bitmask is a no-op and must return success."""
    arc_chip = pyluwen.detect_chips()[asic_id]
    response = send_eth_toggle_reset(arc_chip, 0)
    assert response[0] == 0, (
        f"expected success, got status={response[0]} detail={response[1]}"
    )
    assert response[1] == 0


def test_eth_toggle_reset_individual(arc_chip_dut, asic_id):
    """Reset all individual ETH instances."""
    arc_chip = pyluwen.detect_chips()[asic_id]
    eth_enabled = arc_chip.get_telemetry().enabled_eth

    logger.info("Individually reset each ETH instance without SPI FW reload")
    for eth_inst in range(NUM_ETH):
        set_eth_scratch_pre_reset(arc_chip, (1 << eth_inst) & eth_enabled)
        response = send_eth_toggle_reset(arc_chip, 1 << eth_inst, no_fw_reload=True)
        assert response[0] == 0, (
            f"ETH {eth_inst}: expected success, got status={response[0]} detail={response[1]}"
        )
        assert response[1] == (1 << eth_inst) & eth_enabled
        check_eth_scratch_post_reset(arc_chip, (1 << eth_inst) & eth_enabled)

    logger.info("Individually reset each ETH instance with SPI FW reload")
    for eth_inst in range(NUM_ETH):
        set_eth_scratch_pre_reset(arc_chip, (1 << eth_inst) & eth_enabled)
        response = send_eth_toggle_reset(arc_chip, 1 << eth_inst)
        assert response[0] == 0, (
            f"ETH {eth_inst}: expected success, got status={response[0]} detail={response[1]}"
        )
        assert response[1] == (1 << eth_inst) & eth_enabled
        check_eth_scratch_post_reset(arc_chip, (1 << eth_inst) & eth_enabled)


def test_eth_toggle_reset_all(arc_chip_dut, asic_id):
    """Reset all ETH instances"""
    arc_chip = pyluwen.detect_chips()[asic_id]
    eth_enabled = arc_chip.get_telemetry().enabled_eth
    logger.info("Reset all ETH instances without SPI FW reload")
    set_eth_scratch_pre_reset(arc_chip, ((1 << NUM_ETH) - 1) & eth_enabled)
    response = send_eth_toggle_reset(arc_chip, (1 << NUM_ETH) - 1, no_fw_reload=True)
    assert response[0] == 0, (
        f"expected success, got status={response[0]} detail={response[1]}"
    )
    assert response[1] == eth_enabled
    check_eth_scratch_post_reset(arc_chip, ((1 << NUM_ETH) - 1) & eth_enabled)

    logger.info("Reset all ETH instances with SPI FW reload")
    set_eth_scratch_pre_reset(arc_chip, ((1 << NUM_ETH) - 1) & eth_enabled)
    response = send_eth_toggle_reset(arc_chip, (1 << NUM_ETH) - 1)
    assert response[0] == 0, (
        f"expected success, got status={response[0]} detail={response[1]}"
    )
    assert response[1] == eth_enabled
    check_eth_scratch_post_reset(arc_chip, ((1 << NUM_ETH) - 1) & eth_enabled)


def test_gddr_reset(arc_chip_dut, asic_id):
    """
    Validates the GDDR reset message by toggling MRISC reset on each
    GDDR controller, verifying that BIST passes and TAG_GDDR_STATUS
    reflects the result.
    """

    NUM_GDDR = 8
    TAG_GDDR_STATUS = 22
    GDDR_RESET_ERR_HARVESTED = 2

    arc_chip = pyluwen.detect_chips()[asic_id]

    fail_count = 0
    for gddr_inst in range(NUM_GDDR):
        logger.info(f"Testing GDDR {gddr_inst} reset")

        response = arc_chip.as_bh().arc_msg_buf(
            [TT_SMC_MSG_TOGGLE_GDDR_RESET, gddr_inst, 0, 0, 0, 0, 0, 0]
        )

        status = response[0]
        detail = response[1]

        if status != 0 and detail != GDDR_RESET_ERR_HARVESTED:
            logger.error(f"GDDR {gddr_inst} reset failed with {detail}")
            fail_count += 1
        else:
            logger.info(f"GDDR {gddr_inst} reset passed")

        # Check GDDR telemetry
        if detail != GDDR_RESET_ERR_HARVESTED:
            time.sleep(0.2)
            gddr_status = read_telem(arc_chip, TAG_GDDR_STATUS)
            bist_complete = (gddr_status >> (16 + gddr_inst * 2)) & 1
            bist_failed = (gddr_status >> (17 + gddr_inst * 2)) & 1

            logger.info(f"GDDR {gddr_inst} TAG_GDDR_STATUS=0x{gddr_status:08x} ")

            assert bist_complete == 1, (
                f"GDDR {gddr_inst} BIST complete bit not set in TAG_GDDR_STATUS "
                f"(0x{gddr_status:08x})"
            )

            if status == 0:
                assert bist_failed == 0, (
                    f"GDDR {gddr_inst} BIST failed bit set despite successful reset in "
                    f"TAG_GDDR_STATUS (0x{gddr_status:08x})"
                )
            else:
                assert bist_failed == 1, (
                    f"GDDR {gddr_inst} BIST failed bit not set despite failed reset in "
                    f"TAG_GDDR_STATUS (0x{gddr_status:08x})"
                )

    assert fail_count == 0, f"{fail_count} non-harvested GDDR instances failed reset"


def test_set_tdp_limit(arc_chip_dut, asic_id):
    """
    Validates that the SET_TDP_LIMIT message works

    Sets a TDP limit of 75 W, since that should be lower than the default on all boards.
    Verifies through telemetry that the TDP limit was set correctly.
    Verifies that the TDP limit can be restored to the default value.

    Note that this test currently still works even if the TDP throttler is disabled.
    """
    arc_chip = pyluwen.detect_chips()[asic_id]

    orig_tdp_limit = arc_chip.get_telemetry().tdp_limit_max
    logger.info(f"Original TDP limit: {orig_tdp_limit}")

    # Set TDP limit to 75 watts
    NEW_TDP_LIMIT = 75

    response = arc_chip.as_bh().arc_msg_buf(
        [TT_SMC_MSG_SET_TDP_LIMIT, NEW_TDP_LIMIT, 0, 0, 0, 0, 0, 0]
    )
    assert response[0] == 0, f"Failed to set TDP limit to {NEW_TDP_LIMIT} watts"

    tdp_limit = arc_chip.get_telemetry().tdp_limit_max
    logger.info(f"New TDP limit: {tdp_limit}")

    assert tdp_limit == NEW_TDP_LIMIT, f"TDP limit not set to {NEW_TDP_LIMIT} watts"

    # Set TDP limit to default
    response = arc_chip.as_bh().arc_msg_buf(
        [TT_SMC_MSG_SET_TDP_LIMIT, 0, 1, 0, 0, 0, 0, 0]
    )
    assert response[0] == 0, "Failed to restore default TDP limit"

    tdp_limit = arc_chip.get_telemetry().tdp_limit_max
    logger.info(f"Default TDP limit: {tdp_limit}")

    assert tdp_limit == orig_tdp_limit, (
        f"TDP limit not restored to {orig_tdp_limit} watts"
    )


def test_set_asic_host_fmax(arc_chip_dut, asic_id):
    """
    Validates that the SET_ASIC_HOST_FMAX message works.

    Sets a host fmax of 1000 MHz, which should be within the valid range
    [AICLK_FMAX_MIN=800, AICLK_FMAX_MAX=1400] on all boards.
    Verifies through telemetry (TAG_HOST_AICLK_LIMIT) that the limit was applied.
    Verifies that the limit can be restored to default (disabled).
    Verifies that out-of-range values are rejected.
    """
    arc_chip = pyluwen.detect_chips()[asic_id]

    NEW_HOST_FMAX = 1000  # MHz

    # Positive case: set a valid host fmax
    response = arc_chip.as_bh().arc_msg_buf(
        [TT_SMC_MSG_SET_ASIC_HOST_FMAX, NEW_HOST_FMAX, 0, 0, 0, 0, 0, 0]
    )
    assert response[0] == 0, f"Failed to set host fmax to {NEW_HOST_FMAX} MHz"

    host_fmax = read_telem(arc_chip, TAG_HOST_AICLK_LIMIT)
    logger.info(f"Host fmax telemetry: {host_fmax}")
    assert host_fmax == NEW_HOST_FMAX, (
        f"TAG_HOST_AICLK_LIMIT ({host_fmax}) not set to {NEW_HOST_FMAX} MHz"
    )

    # Positive case: restore default (disables the arbiter)
    response = arc_chip.as_bh().arc_msg_buf(
        [TT_SMC_MSG_SET_ASIC_HOST_FMAX, 0, 1, 0, 0, 0, 0, 0]
    )
    assert response[0] == 0, "Failed to restore default host fmax"

    host_fmax = read_telem(arc_chip, TAG_HOST_AICLK_LIMIT)
    logger.info(f"Host fmax after restore: {host_fmax}")
    assert host_fmax == 0, (
        f"TAG_HOST_AICLK_LIMIT ({host_fmax}) should be 0 after restore_default"
    )

    # Negative case: value too high
    response = arc_chip.as_bh().arc_msg_buf(
        [TT_SMC_MSG_SET_ASIC_HOST_FMAX, 9999, 0, 0, 0, 0, 0, 0]
    )
    assert response[0] != 0, "Expected error for out-of-range fmax (too high)"

    # Negative case: value too low
    response = arc_chip.as_bh().arc_msg_buf(
        [TT_SMC_MSG_SET_ASIC_HOST_FMAX, 100, 0, 0, 0, 0, 0, 0]
    )
    assert response[0] != 0, "Expected error for out-of-range fmax (too low)"


def test_set_characterisation_host_fmin(arc_chip_dut, asic_id):
    """
    Validates that the SET_HOST_REQUESTED_FMIN characterization message works.

    Sets a host fmin of 600 MHz, which should be within the valid range
    [AICLK_FMIN_MIN=200, AICLK_FMIN_MAX=1400] on all boards.
    Verifies that the request succeeds.
    """
    arc_chip = pyluwen.detect_chips()[asic_id]

    NEW_HOST_FMIN = 600  # MHz

    # Positive case: set a valid host fmin
    response = arc_chip.as_bh().arc_msg_buf(
        [
            TT_SMC_MSG_CHARACTERISATION | TT_SUB_MSG_SET_HOST_REQUESTED_FMIN << 8,
            NEW_HOST_FMIN,
            0,
            0,
            0,
            0,
            0,
            0,
        ]
    )
    assert response[0] == 0, f"Failed to set host fmin to {NEW_HOST_FMIN} MHz"
    logger.info(f"Successfully set host fmin to {NEW_HOST_FMIN} MHz")


def test_restore_characterisation_host_fmin(arc_chip_dut, asic_id):
    """
    Validates that restoring default (disabling) the host fmin floor works.

    Uses the restore mechanism (value=1) to disable the host-requested
    minimum frequency floor.
    """
    arc_chip = pyluwen.detect_chips()[asic_id]

    # Positive case: restore default (disables the fmin floor)
    response = arc_chip.as_bh().arc_msg_buf(
        [
            TT_SMC_MSG_CHARACTERISATION | TT_SUB_MSG_SET_HOST_REQUESTED_FMIN << 8,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
        ]
    )
    assert response[0] == 0, "Failed to restore default host fmin"
    logger.info("Successfully restored default host fmin (disabled)")


def test_characterisation_host_fmin_out_of_range(arc_chip_dut, asic_id):
    """
    Validates that out-of-range fmin values are rejected.

    Checks boundaries:
    - Value too high (9999 MHz > 1400): expect error
    - Value too low (100 MHz < 200): expect error
    """
    arc_chip = pyluwen.detect_chips()[asic_id]

    # Negative case: value too high
    response = arc_chip.as_bh().arc_msg_buf(
        [
            TT_SMC_MSG_CHARACTERISATION,
            TT_SUB_MSG_SET_HOST_REQUESTED_FMIN,
            0,
            0,
            9999,
            0,
            0,
            0,
        ]
    )
    assert response[0] != 0, "Expected error for out-of-range fmin (too high)"
    logger.info("Correctly rejected fmin value 9999 (too high)")

    # Negative case: value too low
    response = arc_chip.as_bh().arc_msg_buf(
        [
            TT_SMC_MSG_CHARACTERISATION,
            TT_SUB_MSG_SET_HOST_REQUESTED_FMIN,
            0,
            0,
            100,
            0,
            0,
            0,
        ]
    )
    assert response[0] != 0, "Expected error for out-of-range fmin (too low)"
    logger.info("Correctly rejected fmin value 100 (too low)")


def test_characterisation_kernel_throttler(arc_chip_dut, asic_id):
    """
    Validates the runtime kernel-throttler-at-AICLK-floor characterization messages.

    TAG_KERNEL_THROTTLER telemetry encodes the active config:
    - bit 0: feature enabled
    - bits [31:16]: stop-NOPs frequency in MHz
    """
    arc_chip = pyluwen.detect_chips()[asic_id]

    baseline = read_telem(arc_chip, TAG_KERNEL_THROTTLER)
    logger.info(f"Baseline TAG_KERNEL_THROTTLER: 0x{baseline:08x}")

    def set_enabled(value):
        return arc_chip.as_bh().arc_msg_buf(
            [
                TT_SMC_MSG_CHARACTERISATION
                | TT_SUB_MSG_SET_KERNEL_THROTTLER_ENABLED << 8,
                value,
                0,
                0,
                0,
                0,
                0,
                0,
            ]
        )

    def set_stop_freq(value):
        return arc_chip.as_bh().arc_msg_buf(
            [
                TT_SMC_MSG_CHARACTERISATION
                | TT_SUB_MSG_SET_KERNEL_THROTTLER_STOP_NOPS_FREQ << 8,
                value,
                0,
                0,
                0,
                0,
                0,
                0,
            ]
        )

    # Enable the feature at runtime.
    response = set_enabled(1)
    assert response[0] == 0, "Failed to enable kernel throttler at floor"
    time.sleep(0.2)
    assert read_telem(arc_chip, TAG_KERNEL_THROTTLER) & 1 == 1, (
        "TAG_KERNEL_THROTTLER enabled bit not set after enable"
    )

    # Set a runtime stop frequency within [AICLK_FMIN_MIN=200, AICLK_FMIN_MAX=1400].
    NEW_STOP_FREQ = 800
    response = set_stop_freq(NEW_STOP_FREQ)
    assert response[0] == 0, f"Failed to set stop freq to {NEW_STOP_FREQ} MHz"
    time.sleep(0.2)
    measured = read_telem(arc_chip, TAG_KERNEL_THROTTLER)
    assert (measured >> 16) & 0xFFFF == NEW_STOP_FREQ, (
        f"TAG_KERNEL_THROTTLER stop freq ({(measured >> 16) & 0xFFFF}) "
        f"not set to {NEW_STOP_FREQ} MHz"
    )

    # Out-of-range stop frequencies are rejected.
    assert set_stop_freq(9999)[0] != 0, (
        "Expected error for out-of-range stop freq (too high)"
    )
    assert set_stop_freq(100)[0] != 0, (
        "Expected error for out-of-range stop freq (too low)"
    )

    # Restore the fwtable default stop frequency (value 0).
    response = set_stop_freq(0)
    assert response[0] == 0, "Failed to restore default stop freq"

    # Invalid enable value is rejected.
    assert set_enabled(2)[0] != 0, "Expected error for invalid enable value"

    # Disable the feature again.
    response = set_enabled(0)
    assert response[0] == 0, "Failed to disable kernel throttler at floor"
    time.sleep(0.2)
    assert read_telem(arc_chip, TAG_KERNEL_THROTTLER) & 1 == 0, (
        "TAG_KERNEL_THROTTLER enabled bit still set after disable"
    )

    # Restore the baseline configuration.
    set_enabled(baseline & 1)
    set_stop_freq((baseline >> 16) & 0xFFFF)


def test_bindesc(arc_chip_dut, asic_id):
    """
    Validates that the version descriptor matches what is running on
    the SMC
    """
    arc_chip = pyluwen.detect_chips()[asic_id]
    smc_bin = arc_chip_dut.device_config.app_build_dir / "zephyr/zephyr.bin"
    smc_version = get_ttzp_version.get_ttzp_version_u32(
        SCRIPT_DIR.parents[2] / "app/smc/VERSION"
    )
    logger.info(f"Expected SMC version from file: 0x{smc_version:08x}")
    # Read version from running FW telemetry
    cmfw_version = read_telem(arc_chip, TAG_CM_FW_VERSION)
    assert cmfw_version == smc_version, (
        f"CMFW version mismatch: telemetry 0x{cmfw_version:08x} != expected 0x{smc_version:08x}"
    )
    logger.info(f"CMFW version from telemetry: 0x{cmfw_version:08x}")
    # Read version from bindesc
    output = subprocess.run(
        f"west bindesc dump {smc_bin}".split(),
        capture_output=True,
        text=True,
        check=True,
    )
    # Search stdout for the line formatted as APP_VERSION_STRING: "0.0.0"
    match = re.search(r"v(\d+)\.(\d+)\.(\d+)-.*", output.stdout)
    assert match, "Failed to find version string in bindesc output"
    major, minor, patch = map(int, match.groups())
    bindesc_version = (major << 24) | (minor << 16) | (patch << 8)
    assert bindesc_version == smc_version, (
        f"Bindesc version mismatch: 0x{bindesc_version:08x} != expected 0x{smc_version:08x}"
    )
    logger.info(f"Bindesc version: 0x{bindesc_version:08x}")


def test_ccfgovr_bh_mod(unlaunched_dut: DeviceAdapter, asic_id: int):
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
    bh_mod = shutil.which("bh-mod") or os.path.expanduser("~/bh-mod")
    if not os.path.isfile(bh_mod):
        pytest.skip("bh-mod not found (PATH or ~/bh-mod)")

    # Capture baselines.
    chip = pyluwen.detect_chips()[asic_id]
    tdp_baseline = chip.get_telemetry().tdp_limit_max
    kt_baseline = read_telem(chip, TAG_KERNEL_THROTTLER)
    logger.info(f"Baseline tdp_limit_max: {tdp_baseline}")
    logger.info(f"Baseline kernel_throttler: 0x{kt_baseline:08x}")

    tdp_target = tdp_baseline - 5
    TARGET_STOP_FREQ = 800  # MHz, within [AICLK_FMIN_MIN=200, AICLK_FMIN_MAX=1400]
    kt_expected = 1 | (TARGET_STOP_FREQ << 16)

    # Persist overrides for both the TDP limit and the kernel throttler config.
    result = subprocess.run(
        [
            bh_mod,
            "--reset-timeout=60s",
            "set",
            f"chip_limits.tdp_limit={tdp_target}",
            "feature_enable.kernel_throttler_at_floor_en=true",
            f"chip_limits.kernel_throttler_stop_nops_freq={TARGET_STOP_FREQ}",
        ],
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, f"bh-mod set failed, rc={result.returncode}"

    # Verify the overrides were applied by the running firmware.
    chip = pyluwen.detect_chips()[asic_id]
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

    # Restore the baseline configuration. Warn (don't fail) if this does not
    # succeed, since a stale SPI-flash state can flake later tests / CI runs.
    restore = subprocess.run(
        [
            bh_mod,
            "set",
            f"chip_limits.tdp_limit={tdp_baseline}",
            f"feature_enable.kernel_throttler_at_floor_en={'true' if kt_baseline & 1 else 'false'}",
            f"chip_limits.kernel_throttler_stop_nops_freq={(kt_baseline >> 16) & 0xFFFF}",
        ],
        capture_output=True,
        check=False,
    )
    if restore.returncode != 0:
        logger.warning(
            f"Failed to restore baseline config (rc={restore.returncode}); "
            f"SPI flash may be left modified: {restore.stderr.decode(errors='replace')}"
        )


def test_heartbeat_telemetry(arc_chip_dut, asic_id):
    """
    Polls the heartbeat telemetry every 100ms and verifies that it's incrementing.
    Runs for 2 seconds to collect multiple heartbeat samples.
    """
    arc_chip = pyluwen.detect_chips()[asic_id]

    # Collect heartbeat samples every 100ms
    heartbeat_samples = []
    poll_interval = 0.1  # 100ms in seconds
    duration = 2.0  # 2 seconds total
    end_time = time.time() + duration

    logger.info("Starting heartbeat telemetry polling (100ms intervals for 2 seconds)")

    while time.time() < end_time:
        telemetry = arc_chip.get_telemetry()
        heartbeat = telemetry.timer_heartbeat
        heartbeat_samples.append(heartbeat)
        logger.info(f"Heartbeat: {heartbeat}")
        time.sleep(poll_interval)

    # Verify we collected samples
    assert len(heartbeat_samples) > 0, "No heartbeat samples collected"
    logger.info(f"Collected {len(heartbeat_samples)} heartbeat samples")

    # Verify heartbeat is incrementing
    prev_heartbeat = heartbeat_samples[0]
    for i, heartbeat in enumerate(heartbeat_samples[1:], 1):
        assert heartbeat > prev_heartbeat, (
            f"Heartbeat did not increment: sample {i - 1} ({prev_heartbeat}) >= sample {i} ({heartbeat})"
        )
        prev_heartbeat = heartbeat

    logger.info(
        f"Heartbeat telemetry incrementing correctly: {heartbeat_samples[0]} -> {heartbeat_samples[-1]}"
    )
