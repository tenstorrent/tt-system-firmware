#!/usr/bin/env python3

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
This script dumps SMC state data. It is intended to aid in debugging a hung
SMC. The script requires that the SMC be accessible over PCIe, but does not
require that the application firmware be in a good state.
"""

import argparse
import errno

import pcie_utils

# Register definitions
ICCM_BASE = 0x0
ARC_RESET_UNIT = 0x80030000
SCRATCH_0 = ARC_RESET_UNIT + 0x60
SMC_SCRATCH_RAM_BASE = ARC_RESET_UNIT + 0x400

SMC_POSTCODE_REG = SCRATCH_0
ARC_PC_CORE_0 = ARC_RESET_UNIT + 0x0C00

STRAP_REGISTERS_L = ARC_RESET_UNIT + 0xD20
STRAP_REGISTERS_H = ARC_RESET_UNIT + 0xD24

# Offsets within scratch RAM for named registers
SCRATCH_REGS = {
    "FW Version": 0x00,
    "Boot Status 0": 0x08,
    "Boot Status 1": 0x0C,
    "Error Status 0": 0x10,
    "Error Status 1": 0x14,
    "Message Queue Status": 0x24,
    "SPI Buffer": 0x28,
    "MSG QUEUE INFO": 0x2C,
    "Telemetry Base": 0x30,
    "Telemetry Struct Addr": 0x34,
    "PCIe Init Timestamp": 0x38,
    "CMFW Init Timestamp": 0x3C,
    "DMFW Bootrom Timestamp": 0x40,
    "DMFW Init Duration": 0x44,
    "I2C target state 0": 0x4C,
    "I2C target state 1": 0x50,
    "ARC hang pc": 0x54,
    "Runtime telemetry address": 0x58,
    "Runtime telemetry size": 0x5C,
    "VUART 0 address": 0xA0,
    "VUART 1 address": 0xA4,
    "VUART 2 address": 0xA8,
    "VUART 3 address": 0xAC,
    "VUART 4 address": 0xB0,
    "VUART 5 address": 0xB4,
    "VUART 6 address": 0xB8,
    "VUART 7 address": 0xBC,
    "VUART 8 address": 0xC0,
    "VUART 9 address": 0xC4,
    "VUART 10 address": 0xC8,
    "VUART 11 address": 0xCC,
    "VUART 12 address": 0xD0,
    "VUART 13 address": 0xD4,
    "VUART 14 address": 0xD8,
    "VUART 15 address": 0xDC,
}

# List of all possible states to dump (only these names are valid for --states).
ALL_STATES = ["scratch", "straps", "pc", "crash", "board_id"]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Dump SMC state data for debugging.", allow_abbrev=False
    )
    parser.add_argument(
        "--states",
        type=str,
        nargs="+",
        choices=ALL_STATES,
        default=ALL_STATES,
        metavar="STATE",
        help=(
            "One or more state groups to dump (default: all). "
            f"Valid STATE values: {', '.join(ALL_STATES)}."
        ),
    )
    parser.add_argument(
        "--asic-id",
        type=int,
        nargs="+",
        default=[0],
        metavar="ID",
        help="One or more ASIC indices to dump state from (default: 0).",
    )
    return parser.parse_args()


def _format_board_id_u64(board_id: int) -> str:
    """Format telemetry board_id as unsigned 64-bit hex."""
    return f"0x{(board_id & 0xFFFFFFFFFFFFFFFF):016x}"


def print_board_id_from_telemetry(chip):
    """Print board ID from pyluwen telemetry; tolerate hung / partial firmware."""
    try:
        board_id = chip.get_telemetry().board_id
        print(f"Board ID (telemetry): {_format_board_id_u64(board_id)}")
    except BaseException as e:
        print(f"Board ID (telemetry): unavailable ({e})")


def dump_straps(chip):
    """Dump reset-unit strap GPIO latch registers."""
    print("\nStrap registers:")
    print(f"STRAP_REGISTERS_L: 0x{chip.axi_read32(STRAP_REGISTERS_L):08x}")
    print(f"STRAP_REGISTERS_H: 0x{chip.axi_read32(STRAP_REGISTERS_H):08x}")


def dump_scratch(chip):
    """
    Dump SMC scratch registers
    """
    print("\nSMC Scratch Registers:")
    print(f"SMC Postcode: 0x{chip.axi_read32(SMC_POSTCODE_REG):08x}")
    for name, offset in SCRATCH_REGS.items():
        val = chip.axi_read32(SMC_SCRATCH_RAM_BASE + offset)
        print(f"{name}: 0x{val:08x}")


def dump_crash(chip):
    """Dump crash information that was written to ICCM by the FW's panic handler"""
    print("\nCrash information:")
    print(f"Crash reason: 0x{chip.axi_read32(ICCM_BASE):08x}")
    print(f"Crash BLINK: 0x{chip.axi_read32(ICCM_BASE + 0x4):08x}")


def dump_states(asic_id, states=ALL_STATES):
    """
    Dump specified SMC states from one or more ASICs.

    asic_id may be a single int or an iterable of ints. Callable as a module
    or from the main function.
    """
    asic_ids = [asic_id] if isinstance(asic_id, int) else list(asic_id)
    rc = 0
    for aid in asic_ids:
        print(f"--- ASIC {aid} ---")
        # Don't detect chips with detect_chips(), since that has status checks
        try:
            chip = pcie_utils.get_chip(aid)
        except Exception as e:
            print(f"Error accessing SMC ASIC {aid}: {e}")
            print("Make sure the SMC is powered on and accessible over PCIe")
            rc = errno.EIO
            continue
        if "board_id" in states:
            print_board_id_from_telemetry(chip)
        if "pc" in states:
            pc = chip.axi_read32(ARC_PC_CORE_0)
            print(f"ARC PC: 0x{pc:08x}")
        if "straps" in states:
            dump_straps(chip)
        if "scratch" in states:
            dump_scratch(chip)
        if "crash" in states:
            dump_crash(chip)
        print()
    return rc


def main():
    """
    Main function to dump SMC state
    """
    args = parse_args()
    return dump_states(args.asic_id, states=args.states)


if __name__ == "__main__":
    main()
