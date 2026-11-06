#!/usr/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Check the PCIe link speed/width (and error status) of Tenstorrent devices.

When checking several devices, every device is checked even if an earlier one
fails, and the exit status is non-zero if any failed. Per-device output is also
written to $LOG_DIR/pci-width-<bdf>.log (default: logs).
"""

import argparse
import os
import sys

from pathlib import Path

sys.path.append(str(Path(__file__).parents[1]))

import pcie_utils  # noqa: E402


def main() -> int:
    """Parse the arguments, run the check and return the exit status."""
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument(
        "--board",
        help="check every device against the expectations for this board",
    )
    parser.add_argument("gen", nargs="?", type=int, help="PCIe generation, 1 to 5")
    parser.add_argument(
        "bdf", nargs="?", help="device to check (default: every Tenstorrent device)"
    )
    parser.add_argument(
        "width",
        nargs="?",
        type=int,
        default=pcie_utils.PCIE_DEFAULT_WIDTH,
        help="expected link width (default: %(default)s)",
    )
    args = parser.parse_args()

    log_dir = os.environ.get("LOG_DIR", "logs")
    if args.board:
        expect = pcie_utils.BOARD_PCIE_EXPECTATIONS.get(args.board)
        if expect is None:
            print(f"No PCIe expectations defined for {args.board}, skipping")
            return 0
    elif args.gen is not None:
        expect = {"gen": args.gen, "widths": (args.width,)}
    else:
        parser.error("either --board or a PCIe generation is required")
    ok = pcie_utils.check_all_pcie(expect, args.bdf, log_dir)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
