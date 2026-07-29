#!/usr/bin/env python3

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import argparse
import os
import subprocess
import sys
import yaml
import logging
import time

try:
    import pyluwen

    pyluwen_found = True
except ModuleNotFoundError:
    pyluwen_found = False

from pathlib import Path

import pcie_utils

logger = logging.getLogger(Path(__file__).stem)

DEFAULT_DMC_CFG = (
    Path(__file__).parents[1]
    / "boards/tenstorrent/tt_blackhole/support/tt_blackhole_dmc.cfg"
)
OPT_DIR = Path("/opt/tenstorrent")
SDK_SYSROOT = Path("/opt/zephyr/zephyr-sdk-0.17.0/sysroots/x86_64-pokysdk-linux")

# Execute CMake to locate SDK sysroot
proc = subprocess.run(
    ["cmake", "-P", str(Path(__file__).parent / "find_zephyr_sdk.cmake")],
    capture_output=True,
)
if "SDK_INSTALL_DIR" in str(proc.stderr):
    DEFAULT_SDK_INSTALL_DIR = (
        Path(str(proc.stderr).split(":")[1][:-3])
        / "hosttools"
        / "sysroots"
        / "x86_64-pokysdk-linux"
    )
else:
    DEFAULT_SDK_INSTALL_DIR = SDK_SYSROOT

DEFAULT_OPENOCD = DEFAULT_SDK_INSTALL_DIR / "usr" / "bin" / "openocd"
DEFAULT_SCRIPTS_DIR = DEFAULT_SDK_INSTALL_DIR / "usr" / "share" / "openocd" / "scripts"
DEFAULT_HW_MAP = OPT_DIR / "twister" / "hw-map.yml"


def parse_args():
    parser = argparse.ArgumentParser(description="Reset DMC", allow_abbrev=False)
    parser.add_argument(
        "-c",
        "--config",
        default=DEFAULT_DMC_CFG,
        help="OpenOCD config file",
        metavar="FILE",
        type=Path,
    )
    parser.add_argument(
        "-d",
        "--debug",
        action="count",
        default=0,
        help="Increase debugging verbosity",
    )
    parser.add_argument(
        "-i",
        "--jtag-id",
        help="Specify the JTAG ID / serial number for the debug adapter",
        metavar="ID",
    )
    parser.add_argument(
        "-m",
        "--hw-map",
        default=DEFAULT_HW_MAP,
        help="Use a specific hw-map.yml file",
        metavar="MAP",
        type=Path,
    )
    parser.add_argument(
        "-o",
        "--openocd",
        default=DEFAULT_OPENOCD,
        help="Use a specific openocd executable",
        metavar="FILE",
        type=Path,
    )
    parser.add_argument(
        "-s",
        "--scripts",
        default=DEFAULT_SCRIPTS_DIR,
        help="Path to OpenOCD scripts directory",
        metavar="DIR",
        type=Path,
    )
    parser.add_argument(
        "hexfile",
        help="Hex file to flash, if needed",
        metavar="HEX",
        nargs="?",
        type=Path,
    )
    parser.add_argument(
        "-w",
        "--wait",
        action="store_true",
        help="Wait for SMC to boot after resetting DMC",
    )

    args = parser.parse_args()

    if not args.config.is_file():
        parser.error(f"Config file {args.config} does not exist")
        return None

    if not (args.openocd.is_file() and os.access(str(args.openocd), os.X_OK)):
        parser.error(f"{args.openocd} is not an executable file")
        return None

    if not args.scripts.is_dir():
        parser.error(f"{args.scripts} is not a directory")
        return None

    return args


def reset_dmc(args):
    openocd_cmd = [
        str(args.openocd),
        "-s",
        str(args.scripts),
        "-f",
        str(args.config),
    ]

    if args.jtag_id is not None:
        openocd_cmd.extend(["-c", f"adapter serial {args.jtag_id}"])

    if args.hexfile:
        print(f"Programming file {args.hexfile} to the DMC")
        # program the hex file and reset the DMC
        openocd_cmd.extend(
            [
                "-c",
                "init",
                "-c",
                "targets",
                "-c",
                "reset init",
                "-c",
                f"flash write_image erase {args.hexfile}",
                "-c",
                "reset run; exit",
            ]
        )
    else:
        # simply reset the DMC
        openocd_cmd.extend(["-c", "init", "-c", "targets", "-c", "reset run; exit"])

    if args.debug > 0:
        openocd_kwargs = {
            "stdout": subprocess.PIPE,
            "stderr": subprocess.PIPE,
        }
    else:
        openocd_kwargs = {
            "stdout": subprocess.DEVNULL,
            "stderr": subprocess.DEVNULL,
        }

    proc = subprocess.run(openocd_cmd, **openocd_kwargs)
    if proc.returncode != 0:
        logger.error("command failed: " + " ".join(openocd_cmd))
        logger.error(f"Failed to reset DMC: {proc.stderr}")
        return os.EX_SOFTWARE

    return os.EX_OK


def wait_for_smc_boot(timeout):
    """
    Waits for SMC to complete boot after DMC is reset
    @param timeout: time to wait for boot in seconds
    """
    remaining = timeout
    delay = 1
    # First stage- rescan pcie
    while True:
        try:
            pcie_utils.rescan_pcie()
        except PermissionError:
            return os.EX_OSERR
        if Path("/dev/tenstorrent/0").exists():
            break
        time.sleep(delay)
        remaining -= delay
        if timeout == 0:
            logger.error(f"Card did not enumerate after {timeout} seconds")
            return os.EX_UNAVAILABLE
    # Second stage- is the card firmware working?
    if not pyluwen_found:
        logger.warning(
            "without pyluwen this script can't verify if the SMC firmware is fully working"
        )
        return os.EX_OK
    # Try to detect the card using pyluwen- this indicates ARC has booted
    while True:
        try:
            chips = pyluwen.detect_chips()
            chip = chips[0]
            # Wait for chip to populate telemetry
            telemetry = chip.get_telemetry()
            break
        except Exception:
            # Rescan PCIe again, in case the card disappeared
            pcie_utils.rescan_pcie()
        except BaseException:
            # Rescan PCIe again, in case the card disappeared
            pcie_utils.rescan_pcie()
        remaining -= delay
        time.sleep(delay)
        if remaining == 0:
            logger.error(f"SMC failed to initialize after {timeout} seconds")
            return os.EX_UNAVAILABLE
    # Check if the SMC ping will work
    if telemetry.m3_app_fw_version < 0x40000:
        logger.warning("DMC firmware is too old, no support for SMC ping")
        return os.EX_OK
    # Try to verify that the SMC can ping the DMC
    while True:
        try:
            rsp = chip.arc_msg(pcie_utils.DMC_PING_MSG, True, True, 1, 0)
            if rsp[0] == 1:
                break
        except Exception:
            # Just decrement timeout, which we do below
            pass
        remaining -= delay
        time.sleep(delay)
        if remaining == 0:
            logger.error(f"SMC unable to communicate with DMC after {timeout} seconds")
            return os.EX_UNAVAILABLE
    return os.EX_OK


def main():
    args = parse_args()
    if args is None:
        return os.EX_DATAERR

    if args.debug > 0:
        logging.basicConfig(level=logging.DEBUG)

    if args.jtag_id is None and args.hw_map.is_file():
        logger.debug("Auto-detecting JTAG ID..")

        try:
            hwmap = yaml.load(open(args.hw_map, "r"), Loader=yaml.Loader)
        except Exception as e:
            logger.error(f"Failed to load hardware map: {e}")
            return os.EX_DATAERR

        for dut in hwmap:
            if not dut["connected"]:
                continue
            if dut["platform"].endswith("dmc"):
                setattr(args, "jtag_id", dut["id"])
                logger.debug(f"Found {dut['platform']} with JTAG ID {dut['id']}")
                break

    ret = reset_dmc(args)
    if ret != os.EX_OK:
        return ret
    if args.wait:
        return wait_for_smc_boot(60)
    return os.EX_OK


if __name__ == "__main__":
    sys.exit(main())
