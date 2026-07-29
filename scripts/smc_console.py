#!/usr/bin/env python3

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Script to open console on Tenstorrent SMC devices. Uses tt-fw-terminal
if the executable is available, otherwise falls back to RTT console.
"""

import argparse
from rtt_helper import RTTHelper
from pathlib import Path
import shutil
import subprocess
import sys

DEFAULT_CFG = (
    Path(__file__).parents[1]
    / "boards/tenstorrent/tt_blackhole/support/tt_blackhole_smc.cfg"
)

TT_CONSOLE_SEARCH_DIRS = [
    Path("/opt/tenstorrent/bin"),
    Path(__file__).parents[0] / "tooling",
]
DEFAULT_SEARCH_BASE = 0x10000000
DEFAULT_SEARCH_RANGE = 0x80000


def start_smc_rtt():
    """
    Function to start SMC RTT console
    """
    rtt_helper = RTTHelper(
        cfg=DEFAULT_CFG,
        search_base=DEFAULT_SEARCH_BASE,
        search_range=DEFAULT_SEARCH_RANGE,
    )
    args = sys.argv[1:].copy()
    if "--rtt" in args:
        args.remove("--rtt")
    rtt_helper.parse_args(args)
    rtt_helper.run_rtt_server()


def find_tt_console():
    """
    Find the tt-fw-terminal executable in the specified search directories.
    """
    if shutil.which("tt-fw-terminal"):
        return shutil.which("tt-fw-terminal")
    # Check each directory in TT_CONSOLE_SEARCH_DIRS
    for search_dir in TT_CONSOLE_SEARCH_DIRS:
        console_exec = search_dir / "tt-fw-terminal"
        if console_exec.exists() and shutil.which(str(console_exec)):
            return str(console_exec)
    return None


def tt_card_on_bus():
    """
    Check if a Tenstorrent card is present on the bus.
    """
    return Path("/dev/tenstorrent/0").exists()


def parse_args():
    """
    Parse command line arguments.
    """
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument(
        "--rtt",
        action="store_true",
        help="Force use of RTT console",
    )
    return parser.parse_known_args()[0]


def main():
    """
    Main function to start the SMC console.
    """
    args = parse_args()
    console_exec = find_tt_console()

    if not tt_card_on_bus() or console_exec is None or args.rtt:
        print("Using RTT console")
        start_smc_rtt()
    else:
        # If tt-fw-terminal is available and a card is present, use it.
        print(f"Using tt-fw-terminal at {console_exec}")
        try:
            subprocess.run([console_exec] + sys.argv[1:])
        except KeyboardInterrupt:
            print("Exiting tt-fw-terminal")


if __name__ == "__main__":
    main()
