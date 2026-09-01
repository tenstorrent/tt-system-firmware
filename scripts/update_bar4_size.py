#!/usr/bin/env python3

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Update BAR4 size in firmware bundle.

This script modifies the pcie_bar4_size field in the pci0_property_table and
pci1_property_table structures within the cmfwcfg (firmware configuration table)
stored in a firmware bundle.

Usage:
    # List available boards in a firmware bundle
    ./update-bar4-size.py --input fw_pack.fwbundle --board ?

    # Update BAR4 size to 32768 MiB for a specific board
    ./update-bar4-size.py --input fw_pack.fwbundle --output fw_pack_out.fwbundle --board P100A-1 --size 32768

    # Update BAR4 size for all boards in the bundle
    ./update-bar4-size.py --input fw_pack.fwbundle --output fw_pack_out.fwbundle --size 32768

    # Disable BAR4 (set to 0)
    ./update-bar4-size.py --input fw_pack.fwbundle --output fw_pack_out.fwbundle --board P100A-1 --size 0
"""

import os
import logging
import sys
import argparse

from pathlib import Path

BOOTFS_FWTABLE_NAME = "cmfwcfg"

TT_Z_P_ROOT = Path(__file__).parents[1]
sys.path.append(str(TT_Z_P_ROOT / "scripts"))

import tt_boot_fs  # noqa: E402
import fwtable_tooling  # noqa: E402

_logger = logging.getLogger(__name__)


def parse_args():
    parser = argparse.ArgumentParser(description="Update BAR4 size", allow_abbrev=False)
    parser.add_argument(
        "--board",
        type=str,
        help=(
            "Name of the board to update BAR4 size for. Can be specified multiple times. "
            "If not specified, all boards will be updated. "
            "If '?' is specified then available board names are listed."
        ),
        action="append",
    )
    parser.add_argument(
        "--bus",
        type=int,
        help=(
            "Number of the bus to update BAR4 size for. Can be specified multiple times. "
            "If not specified, all busses will be updated. "
        ),
        action="append",
    )
    parser.add_argument(
        "--input",
        type=Path,
        required=True,
        help="Path to the input firmware bundle",
    )
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="Path to the output firmware bundle",
    )
    parser.add_argument(
        "--size",
        type=int,
        default=0,
        help="The new power-of-two size for BAR4 in MiB (0 to disable). Requires a cold reboot to take effect",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Enable verbose logging",
    )
    args = parser.parse_args()

    return args


def iterate_bar4_sizes(
    cb_object,
    fw_table_pb2_pkg,
    bootfs_bin_path: Path,
    verify: bool = False,
    verbose: bool = False,
):
    bootfs_b16_path = bootfs_bin_path.with_suffix(".b16")
    os.rename(bootfs_bin_path, bootfs_b16_path)
    bootfs_bytes = tt_boot_fs.extract_all(bootfs_b16_path, input_base64=True)

    bootfs = tt_boot_fs.BootFs.from_binary(bootfs_bytes)
    table_idx = fwtable_tooling.get_cmfwcfg_table(bootfs)
    if table_idx is None:
        return os.EX_DATAERR
    cmfwcfg_entry = bootfs.tables[table_idx].entries[BOOTFS_FWTABLE_NAME]

    cmfwcfg_addr = cmfwcfg_entry.spi_addr
    cmfwcfg_size = len(cmfwcfg_entry.data)
    cmfwcfg_bytes = cmfwcfg_entry.data
    _logger.debug(
        f"{BOOTFS_FWTABLE_NAME} at SPI address: 0x{cmfwcfg_addr:x}, size: {cmfwcfg_size} bytes"
    )

    size = cb_object["size"]

    if size < 0:
        _logger.error("Size may not be negative")
        return os.EX_DATAERR

    if size > 0 and size & (size - 1) != 0:
        _logger.error("Non-zero size must be a power of two")
        return os.EX_DATAERR

    if not cb_object["bus"]:
        # No bus numbers have been supplied, so use all of them that apply
        busses = []
    else:
        busses = cb_object["bus"]
    # sort bus numbers so that it's deterministic to cycle through them
    busses = sorted(busses)

    # Remove nanopb framing
    if verbose:
        tt_boot_fs.hexdump(cmfwcfg_addr, cmfwcfg_bytes, checksum=True)
    proto_bytes = fwtable_tooling.nanopb_remove_framing(cmfwcfg_bytes)

    # Parse the protobuf message
    fw_table = fw_table_pb2_pkg.FwTable()
    fw_table.ParseFromString(proto_bytes)

    def safe_has_field(table, field_name: str) -> bool:
        try:
            return table.HasField(field_name)
        except ValueError:
            return False

    # filter busses. The assumption is that bus N+1 only exists if bus N exists
    bus = 0
    filtered_busses = []
    while safe_has_field(fw_table, f"pci{bus}_property_table"):
        if not busses or bus in busses:
            filtered_busses.append(bus)
        bus += 1
    busses = filtered_busses

    if verify:
        # Just verify that the sizes are as expected
        for bus in busses:
            pci_table = getattr(fw_table, f"pci{bus}_property_table")
            if pci_table.pcie_bar4_size != size:
                raise ValueError(
                    f"Verification failed: pci{bus}_property_table.pcie_bar4_size is {pci_table.pcie_bar4_size} MiB, expected {size} MiB"
                )
            _logger.info(
                f"Verified pci{bus}_property_table.pcie_bar4_size is {pci_table.pcie_bar4_size} MiB"
            )
        return os.EX_OK

    # Update BAR4 size
    for bus in busses:
        pci_table = getattr(fw_table, f"pci{bus}_property_table")
        _logger.debug(
            f"Current pci{bus}_property_table.pcie_bar4_size: {pci_table.pcie_bar4_size} MiB"
        )
        pci_table.pcie_bar4_size = size
        _logger.debug(f"Updated pci{bus}_property_table.pcie_bar4_size to {size} MiB")

    # Serialize back to bytes
    proto_bytes = fw_table.SerializeToString()

    # Add nanopb framing
    # The calls below can be uncommented to see how deframing works
    # tt_boot_fs.hexdump(cmfwcfg_addr, proto_bytes, checksum=True)
    cmfwcfg_bytes = fwtable_tooling.nanopb_add_framing(proto_bytes)
    if verbose:
        tt_boot_fs.hexdump(cmfwcfg_addr, cmfwcfg_bytes, checksum=True)

    # Update the bootfs with the new cmfwcfg
    bootfs.tables[table_idx].entries[BOOTFS_FWTABLE_NAME].data = cmfwcfg_bytes

    bootfs_bytes = bootfs.to_binary(True)

    with open(bootfs_bin_path, "wb") as f:
        # Convert back to the b16 encoded format
        f.write(bytes(bootfs.to_b16(), "ascii"))

    os.remove(bootfs_b16_path)
    return os.EX_OK


def main():
    args = parse_args()

    cb_object = {
        "bus": args.bus,
        "size": args.size,
    }

    return fwtable_tooling.do_update(
        args.input,
        args.output,
        args.board,
        iterate_bar4_sizes,
        cb_object,
        args.verbose,
    )


if __name__ == "__main__":
    sys.exit(main())
