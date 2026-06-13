#!/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

"""
This script flashes firmware onto a wormhole (n300 or n150) card using a recovery method.
An STLink must be connected to the STM32 on the card in order to perform the recovery.
"""

import argparse
from datetime import date
from intelhex import IntelHex
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import time
import os

try:
    from pyocd.flash.file_programmer import FileProgrammer
except ImportError:
    print("Required modules not found. Please run pip install pyocd")
    sys.exit(os.EX_UNAVAILABLE)

sys.path.append(str(Path(__file__).parents[2]))
import pcie_utils
import pyocd_utils

PYOCD_TARGET = "stm32g031c6ux"
WH_FLM_DIR = Path(__file__).parent / "data" / "wh_flm"

SPI_CONFIGS = {
    "spi1": WH_FLM_DIR / "pyocd_config_spi1.py",
    "spi2": WH_FLM_DIR / "pyocd_config_spi2.py",
}

# Board type: (firmware name in bundle, list of SPIs to flash)
BOARD_CONFIG = {
    "n300": ("NEBULA_X2", ["spi1", "spi2"]),
    "n150": ("NEBULA_X1", ["spi2"]),
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Utility to recover wormhole card", allow_abbrev=False
    )
    parser.add_argument(
        "bundle",
        help="Path to firmware bundle (.fwbundle)",
    )
    parser.add_argument(
        "board",
        type=str,
        choices=["n300", "n150"],
        help="Board type to recover",
    )
    parser.add_argument(
        "--board-id",
        type=str,
        default="auto",
        help="Board ID to program into the board's EEPROM",
    )
    parser.add_argument("--adapter-id", help="Adapter ID to use for pyocd")
    parser.add_argument(
        "--force",
        action="store_true",
        help="Forcibly recover the card, even if sanity checks pass",
    )
    parser.add_argument(
        "--no-prompt",
        action="store_true",
        help="Don't prompt for adapter if multiple are found",
    )
    return parser.parse_args()


def extract_fwbundle(bundle_path, boardname):
    """Open a .fwbundle and return (image_bytes, mask, mapping, manifest)."""
    fw_package = tarfile.open(bundle_path, "r")

    manifest_data = fw_package.extractfile("./manifest.json")
    image_raw = fw_package.extractfile(f"./{boardname}/image.bin")
    mask_raw = fw_package.extractfile(f"./{boardname}/mask.json")
    mapping_raw = fw_package.extractfile(f"./{boardname}/mapping.json")

    if manifest_data is None:
        raise ValueError(f"manifest.json not found in {bundle_path}")
    if image_raw is None:
        raise ValueError(f"{boardname}/image.bin not found in bundle")
    if mask_raw is None:
        raise ValueError(f"{boardname}/mask.json not found in bundle")
    if mapping_raw is None:
        raise ValueError(f"{boardname}/mapping.json not found in bundle")

    image_bytes = image_raw.read()
    mask = json.loads(mask_raw.read())
    mapping = json.loads(mapping_raw.read())
    manifest = json.loads(manifest_data.read())

    return (image_bytes, mask, mapping, manifest)


def create_fw_hex(bundle_path, boardname, board_id):
    """
    Extract the fw data for board type boardname from the bundle_path
    Apply patches from mask.json, ignoring all 'rmw' field except
    BOARD_INFO. Write board_id into BOARD_INFO.
    Returns IntelHex of the fw to write.
    """
    image_bytes, mask, mapping, manifest = extract_fwbundle(bundle_path, boardname)

    def _incr(data, data_addr, length):
        # Resets REPROGRAMMED_COUNT to 0
        data[data_addr : data_addr + length] = bytes(length)
        return data

    def _date(data, data_addr, length):
        # Sets DATE_PROGRAMMED to today
        today = date.today()
        int_date = int(f"0x{today.strftime('%Y%m%d')}", 16)
        data[data_addr : data_addr + length] = int_date.to_bytes(length, "little")
        return data

    def _flash_version(data, data_addr, length):
        # Sets FLASH_VERSION (tool version) to 0, since we're flashing via recovery, not tt-flash
        data[data_addr : data_addr + length] = bytes(length)
        return data

    def _bundle_version(data, data_addr, length):
        version_parts = manifest.get("bundle_version", {})
        version = [
            version_parts.get("debug", 0),
            version_parts.get("patch", 0),
            version_parts.get("releaseId", 0),
            version_parts.get("fwId", 0),
        ]
        data[data_addr : data_addr + length] = bytes(version)
        return data

    TAG_HANDLERS = {
        "incr": _incr,
        "date": _date,
        "flash_version": _flash_version,
        "bundle_version": _bundle_version,
    }

    param_handlers = []
    for entry in mask:
        tag = entry.get("tag")
        if tag == "rmw":
            # Skip rmw in recovery mode
            continue
        handler = TAG_HANDLERS.get(tag)
        if handler is None:
            print(f"Warning: unknown mask tag {tag}, skipping")
            continue
        param_handlers.append(((entry["start"], entry["end"]), handler))

    # Parse sparse-hex image.bin, apply patches, and load into IntelHex
    ih = IntelHex()
    curr_addr = 0
    for line in image_bytes.decode("utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("@"):
            curr_addr = int(line.lstrip("@").strip())
        else:
            chunk = bytearray.fromhex(line)
            curr_stop = curr_addr + len(chunk)
            for (start, end), handler in param_handlers:
                if start < curr_stop and end > curr_addr:
                    chunk = handler(chunk, start - curr_addr, end - start)
            ih.puts(curr_addr, bytes(chunk))
            curr_addr = curr_stop

    # Write board_id into BOARD_INFO (replaces the skipped rmw field)
    board_info = mapping.get("HEADER", {}).get("BOARD_INFO", {})
    start = board_info.get("start")
    end = board_info.get("end")
    if start is None or end is None:
        print(
            "Warning: BOARD_INFO location not found in mapping.json - board ID not written"
        )
    else:
        length = end - start
        ih.puts(start, board_id.to_bytes(length, "little"))
        print(f"Wrote board ID at @ 0x{start:05x}: 0x{board_id:016x}")

    return ih


def check_card_status():
    """Check if the card is in a good state"""
    # TODO: assumes one card only on the system (ok for both n150 and n300)
    # TODO: perform health check with luwen?
    # See if the card is on the bus
    if not Path("/dev/tenstorrent/0").exists():
        print("Card not found on bus")
        return False
    return True


def flash_spi(spi, fw_bin, adapter_id, no_prompt):
    """Erase, program, and reset the M3 for one SPI flash."""
    pyocd_config = SPI_CONFIGS[spi]
    session = pyocd_utils.open_session(
        pyocd_config, adapter_id, no_prompt, PYOCD_TARGET
    )
    try:
        FileProgrammer(session).program(str(fw_bin), file_format="hex")
        session.board.target.reset_and_halt()
        session.board.target.resume()
    finally:
        session.close()


def main():
    args = parse_args()
    bundle_path = Path(args.bundle)
    if not bundle_path.exists():
        print(f"Error: bundle not found: {bundle_path}")
        sys.exit(1)

    boardname, spis = BOARD_CONFIG[args.board]

    if args.board_id == "auto":
        if args.board == "n300":
            board_id = 0x14 << 36
        elif args.board == "n150":
            board_id = 0x18 << 36
    else:
        board_id = int(args.board_id, 0)

    if not args.force and check_card_status():
        print(f"All ASICs on board {args.board} are functional, skipping recovery")
        print("Use --force to perform recovery anyway")
        return

    print(f"Preparing {boardname} firmware from bundle...")
    print("Note: board-specific data (rmw patches) will not be preserved")
    with tempfile.TemporaryDirectory() as temp_dir:
        fw_hex = create_fw_hex(bundle_path, boardname, board_id)
        fw_hex_path = Path(temp_dir) / f"{boardname}_recovery.hex"
        fw_hex.write_hex_file(str(fw_hex_path))

        for spi in spis:
            print(f"Flashing {spi} on {boardname}...")
            flash_spi(spi, fw_hex_path, args.adapter_id, args.no_prompt)

    print("Waiting 20 seconds for M3 to update...")
    pcie_utils.rescan_pcie()
    timeout = 20  # seconds
    timeout_ts = time.time() + timeout
    while time.time() < timeout_ts:
        if check_card_status():
            print("Card recovered successfully")
            return
        # Otherwise, try rescanning the PCIe bus
        pcie_utils.rescan_pcie()
        # Wait a bit and try again
        time.sleep(1)
    # If we get here, the card did not recover
    raise RuntimeError("Card did not recover successfully, try a power cycle?")


if __name__ == "__main__":
    main()
