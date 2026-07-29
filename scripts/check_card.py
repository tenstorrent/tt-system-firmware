#!/usr/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import argparse
import logging
import sys

from pathlib import Path

import pcie_utils

logger = logging.getLogger(Path(__file__).stem)


def main():
    parser = argparse.ArgumentParser(
        description="Wait for a Tenstorrent card to enumerate and respond to ARC/DMC pings",
        allow_abbrev=False,
    )
    parser.add_argument("--verbose", action="store_true", help="Enable verbose logging")
    parser.add_argument(
        "--asic-id", type=int, default=0, help="ASIC index (default: 0)"
    )
    parser.add_argument(
        "--timeout", type=int, default=60, help="Timeout in seconds (default: 60)"
    )

    args = parser.parse_args()

    level = logging.DEBUG if args.verbose else logging.INFO
    logging.basicConfig(level=level, format="%(name)s: %(levelname)s: %(message)s")

    ok = pcie_utils.wait_for_enum(asic_id=args.asic_id, timeout=args.timeout)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
