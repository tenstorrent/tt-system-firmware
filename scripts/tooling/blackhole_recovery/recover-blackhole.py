#!/bin/env python3

# Copyright (c) 2025 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

"""
This script flashes firmware from a "recovery bundle" onto a blackhole card.
The bundle should be created using the "prepare-recovery-bundle.py" script.
"""

import argparse
from pathlib import Path
import tarfile
import tempfile
import sys
import time
import os
from intelhex import IntelHex

try:
    import pyluwen
    from pyocd.flash.file_programmer import FileProgrammer
    from pyocd.flash.eraser import FlashEraser
except ImportError:
    print("Required modules not found. Please run pip install -r requirements.txt")
    sys.exit(os.EX_UNAVAILABLE)

sys.path.append(str(Path(__file__).parents[2]))
# Local imports
import pyocd_utils
import pcie_utils
import tt_boot_fs

# Set environment variable for protobuf implementation
if os.environ.get("PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION") != "python":
    os.environ["PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION"] = "python"
from encode_spirom_bins import convert_proto_txt_to_bin_file

TT_Z_P_ROOT = Path(__file__).parents[3]

SMC_SCRATCH_RAM_BASE = 0x80030400
TELEMETRY_DATA_REG_ADDR = SMC_SCRATCH_RAM_BASE + 12 * 4
TELEMETRY_TABLE_REG_ADDR = SMC_SCRATCH_RAM_BASE + 13 * 4
TAG_BOARD_ID_EXT_HIGH = 81
TAG_BOARD_ID_EXT_MIDDLE = 82
TAG_BOARD_ID_EXT_LOW = 83
MAX_TELEMETRY_ENTRIES = 1024


def parse_args():
    parser = argparse.ArgumentParser(
        description="Utility to recover blackhole card", allow_abbrev=False
    )
    parser.add_argument(
        "bundle",
        help="Filename of recovery bundle",
    )
    parser.add_argument("board", help="Board name to recover")
    parser.add_argument(
        "--board-id",
        type=str,
        default="auto",
        help="Board ID to program into the board's EEPROM",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Forcibly recover the card, even if sanity checks pass",
    )
    pyocd_utils.add_common_args(parser, verbose=False)
    return parser.parse_args()


def set_board_serial(hex_file, board_name, board_id, extended=False):
    """
    Modifies a recovery hex file to include a new board ID in the EEPROM image
    @param hex_file: Path to hex file to patch. Should include tt-boot-fs and dmc image
    @param board_name: Name of board in protobuf definitions
    @param board_id: serial to patch for board
    @param extended: always write board_id_high, marking the ID as extended
    """
    try:
        import read_only_pb2  # pylint: disable=import-outside-toplevel
    except ImportError as e:
        print(f"Error importing protobuf modules: {e}")
        print("Ensure the protobuf files are generated and the path is correct.")
        sys.exit(1)
    work_dir = Path(hex_file).parent

    override = {"board_id": board_id & 0xFFFFFFFFFFFFFFFF}
    # Presence of board_id_high marks an extended ID, even when it is 0
    if extended or board_id >> 64:
        override["board_id_high"] = board_id >> 64

    # Dump new readonly data
    convert_proto_txt_to_bin_file(
        TT_Z_P_ROOT / "boards" / "tenstorrent" / "tt_blackhole" / "spirom_data_tables",
        board_name,
        work_dir / "generated_proto_bins",
        "read_only",
        read_only_pb2.ReadOnly,
        False,
        override=override,
    )
    bootfs_data = IntelHex(hex_file).tobinstr()
    protobuf_bin = open(
        work_dir / "generated_proto_bins" / board_name / "read_only.bin", "rb"
    ).read()
    bootfs_entries = tt_boot_fs.BootFs.from_binary(bootfs_data).entries
    boardcfg = bootfs_entries["boardcfg"]
    # Recreate boardcfg entry with the protobuf data
    order = list(bootfs_entries.keys())
    order.remove("failover")
    failover = bootfs_entries["failover"]
    bootfs_entries["boardcfg"] = tt_boot_fs.FsEntry(
        False, "boardcfg", protobuf_bin, boardcfg.spi_addr, 0x0, False
    )
    bootfs_data = tt_boot_fs.BootFs(order, bootfs_entries, failover).to_intel_hex(True)
    with open(work_dir / "new_serial.hex", "wb") as f:
        f.write(bootfs_data)
    original = IntelHex(hex_file)
    new = IntelHex(str(work_dir / "new_serial.hex"))
    original.merge(new, overlap="replace")
    original.write_hex_file(hex_file)
    return hex_file


def default_board_id(asic):
    if asic.get("extended-board-id"):
        # Extended IDs carry the UPI in hex digits 1-5 of the leading PN
        return asic["upi"] << 52
    return asic["upi"] << 36 | (0x1 << 32)


def is_extended_board_id(asic, board_id):
    return bool(asic.get("extended-board-id")) or bool(board_id >> 64)


def expected_board_ids(board_config, board_id):
    if board_id == "auto":
        return [default_board_id(asic) for asic in board_config]
    parsed_id = int(board_id, 0)
    if parsed_id < 0 or parsed_id >= (1 << 80):
        raise ValueError("Board ID must fit in 80 bits")
    return [parsed_id] * len(board_config)


def read_telemetry_tags(card, tags):
    """Read telemetry tags directly from the SMC telemetry table over AXI."""
    table_addr = card.axi_read32(TELEMETRY_TABLE_REG_ADDR)
    data_addr = card.axi_read32(TELEMETRY_DATA_REG_ADDR)
    if table_addr in (0, 0xFFFFFFFF) or data_addr in (0, 0xFFFFFFFF):
        raise RuntimeError("telemetry not initialized")
    entry_count = card.axi_read32(table_addr + 0x4)
    if entry_count > MAX_TELEMETRY_ENTRIES:
        raise RuntimeError(f"implausible telemetry entry_count: {entry_count}")

    values = {}
    for i in range(entry_count):
        # struct telemetry_entry { uint16_t tag; uint16_t offset; }
        raw = card.axi_read32(table_addr + 0x8 + i * 4)
        tag = raw & 0xFFFF
        if tag in tags:
            values[tag] = card.axi_read32(data_addr + ((raw >> 16) & 0xFFFF) * 4)
    missing = set(tags) - values.keys()
    if missing:
        raise RuntimeError(f"telemetry tags {sorted(missing)} not reported by firmware")
    return values


def read_extended_board_id(card):
    values = read_telemetry_tags(
        card, (TAG_BOARD_ID_EXT_HIGH, TAG_BOARD_ID_EXT_MIDDLE, TAG_BOARD_ID_EXT_LOW)
    )
    return (
        (values[TAG_BOARD_ID_EXT_HIGH] << 64)
        | (values[TAG_BOARD_ID_EXT_MIDDLE] << 32)
        | values[TAG_BOARD_ID_EXT_LOW]
    )


def cards_on_bus(board_config):
    """Return True only if every ASIC's device node is present on the PCIe bus."""
    return all(
        Path(f"/dev/tenstorrent/{pci_idx}").exists()
        for pci_idx in range(len(board_config))
    )


def check_card_status(board_config, expected_ids=None):
    """Check if the card is in a good state"""
    for pci_idx, config in enumerate(board_config):
        # See if the card is on the bus
        if not Path(f"/dev/tenstorrent/{pci_idx}").exists():
            print(f"Card {pci_idx} not found on bus")
            return False
        # Check if the card can be accessed by pyluwen
        try:
            card = pyluwen.detect_chips()[pci_idx]
            if not pcie_utils.ping_arc(card, pci_idx):
                return False
            if not pcie_utils.ping_dmc(card, pci_idx):
                return False
            expected_id = expected_ids[pci_idx] if expected_ids else None
            if expected_id is not None and is_extended_board_id(config, expected_id):
                actual_id = read_extended_board_id(card)
                if actual_id != expected_id:
                    print(
                        f"Extended board ID 0x{actual_id:020x} does not match "
                        f"expected 0x{expected_id:020x} for ASIC {pci_idx}"
                    )
                    return False
            elif card.get_telemetry().board_id >> 36 != config["upi"]:
                print(f"Board ID UPI does not match expected value for ASIC {pci_idx}")
                return False
        except Exception as e:
            print(f"Error accessing card with pyluwen for ASIC {pci_idx}: {e}")
            return False
    return True


def main():
    args = parse_args()
    with (
        tempfile.TemporaryDirectory() as temp_dir,
        tarfile.open(args.bundle, "r:gz") as tar,
    ):
        tar.extractall(path=temp_dir, filter="data")
        try:
            BOARD_ID_MAP = pyocd_utils.load_board_metadata(
                Path(temp_dir) / "board_metadata.yaml"
            )
        except FileNotFoundError:
            print("Error: board_metadata.yaml not found in recovery bundle")
            return

        if args.board not in BOARD_ID_MAP:
            print(f"Error: board {args.board} not found in recovery bundle")
            return

        expected_ids = expected_board_ids(BOARD_ID_MAP[args.board], args.board_id)
        sys.path.append(str(Path(temp_dir) / args.board / "python_proto_files"))

        if (not args.force) and check_card_status(
            BOARD_ID_MAP[args.board], expected_ids
        ):
            print(f"All ASICs on board {args.board} are functional, skipping recovery")
            return

        for idx in range(len(BOARD_ID_MAP[args.board])):
            asic = BOARD_ID_MAP[args.board][idx]
            pyocd_config = Path(temp_dir) / asic["pyocd-config"]
            session = pyocd_utils.get_session(
                pyocd_config,
                args.adapter_id,
                args.no_prompt,
                asic.get("pyocd-target"),
            )
            session.open()
            # First, reset the DMC and see if we can reach the card
            session.board.target.reset_and_halt()
            session.board.target.resume()
            session.close()
            time.sleep(2)
            pcie_utils.rescan_pcie()
            time.sleep(2)

        if (not args.force) and check_card_status(
            BOARD_ID_MAP[args.board], expected_ids
        ):
            print(f"All ASICs on board {args.board} are functional, skipping recovery")
            return

        # First, erase the flash on all ASICs
        for idx in range(len(BOARD_ID_MAP[args.board])):
            asic = BOARD_ID_MAP[args.board][idx]
            pyocd_config = Path(temp_dir) / asic["pyocd-config"]
            session = pyocd_utils.get_session(
                pyocd_config,
                args.adapter_id,
                args.no_prompt,
                asic.get("pyocd-target"),
            )
            session.open()
            # Reset and halt the DMC so its firmware is not running on (and
            # holding) the shared SPI bus while we drive the flash loader
            session.board.target.reset_and_halt()
            # Erase the flash
            print(f"Erasing flash on ASIC {idx}...")
            FlashEraser(session, FlashEraser.Mode.CHIP).erase()
            session.close()
            time.sleep(1)

        # Now, program the new binaries to all ASICs
        for idx in range(len(BOARD_ID_MAP[args.board])):
            asic = BOARD_ID_MAP[args.board][idx]
            recovery_hex = (
                Path(temp_dir) / args.board / f"{asic['bootfs-name']}_recovery.hex"
            )
            if not recovery_hex.exists():
                print(f"Error: recovery hex file {recovery_hex} not found in bundle")
                return
            if args.board_id == "auto":
                board_id = default_board_id(asic)
            else:
                board_id = int(args.board_id, 0)
            recovery_hex = set_board_serial(
                str(recovery_hex),
                asic["protobuf-name"],
                board_id,
                extended=is_extended_board_id(asic, board_id),
            )
            pyocd_config = Path(temp_dir) / asic["pyocd-config"]
            session = pyocd_utils.get_session(
                pyocd_config,
                args.adapter_id,
                args.no_prompt,
                asic.get("pyocd-target"),
            )
            session.open()
            # Reset and halt the DMC before programming for the same reason as
            # the erase loop: keep firmware off the shared SPI bus
            session.board.target.reset_and_halt()
            # Program the recovery hex
            print(f"Flashing {recovery_hex} to ASIC {idx}...")
            FileProgrammer(session).program(str(recovery_hex), file_format="hex")
            session.board.target.reset_and_halt()
            session.board.target.resume()
            session.close()
        # After flashing, the card resets and the DMFW self-updates. That cycle
        # takes the PCIe endpoint down and back up, so the card transiently drops
        # off the bus before re-enumerating. Poll until it comes back and is fully
        # responsive. Start with one bus rescan, then only rescan again when the
        # card is actually missing: rescanning removes and re-adds the device,
        # which can race with the card's own re-enumeration if the link is still training.
        print("Waiting for DMFW update and PCIe re-enumeration...")
        timeout = 30  # seconds; DMFW update + double PCIe link train can be slow
        timeout_ts = time.time() + timeout
        pcie_utils.rescan_pcie()
        while time.time() < timeout_ts:
            if check_card_status(BOARD_ID_MAP[args.board], expected_ids):
                print("Card recovered successfully")
                return
            # Only rescan when the card is off the bus; otherwise give the link
            # time to finish training and the card to become responsive.
            if not cards_on_bus(BOARD_ID_MAP[args.board]):
                pcie_utils.rescan_pcie()
            time.sleep(2)
        # If we get here, the card did not recover
        raise RuntimeError("Card did not recover successfully, try a reboot?")


if __name__ == "__main__":
    main()
