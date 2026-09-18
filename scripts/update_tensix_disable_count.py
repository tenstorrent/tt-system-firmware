#!/usr/bin/env python3

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Update Tensix column disable count in firmware bundle.

This script modifies the tensix_disable_count field in the product_spec_harvesting
structure within the cmfwcfg (firmware configuration table)
stored in a firmware bundle.

Usage:
    # List available boards in a firmware bundle
    ./update_tensix_disable_count.py --input fw_pack.fwbundle --board ?

    # Set tensix_disable_count for a specific board
    ./update_tensix_disable_count.py --input fw_pack.fwbundle --output fw_pack_out.fwbundle --board P100A-1 --disable-count 2

    # Set tensix_disable_count for all boards in the bundle
    ./update_tensix_disable_count.py --input fw_pack.fwbundle --output fw_pack_out.fwbundle --disable-count 2

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
    parser = argparse.ArgumentParser(
        description="Update Tensix Harvest Count", allow_abbrev=False
    )
    parser.add_argument(
        "--board",
        type=str,
        help=(
            "Name of the board to update Tensix Harvest Count for. Can be specified multiple times. "
            "If not specified, all boards will be updated. "
            "If '?' is specified then available board names are listed."
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
        "--disable-count",
        type=int,
        default=0,
        help="The new Tensix disable count. Takes effect after flashing the updated firmware.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Enable verbose logging",
    )
    args = parser.parse_args()

    return args


def set_tensix_disable_count(
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

    disable_count = cb_object["disable_count"]

    # Remove nanopb framing
    if verbose:
        tt_boot_fs.hexdump(cmfwcfg_addr, cmfwcfg_bytes, checksum=True)
    proto_bytes = fwtable_tooling.nanopb_remove_framing(cmfwcfg_bytes)

    # Parse the protobuf message
    fw_table = fw_table_pb2_pkg.FwTable()
    fw_table.ParseFromString(proto_bytes)

    if verify:
        # Just verify that the sizes are as expected
        product_spec = getattr(fw_table, "product_spec_harvesting")
        if product_spec.tensix_col_disable_count != disable_count:
            raise ValueError(
                f"Verification failed: product_spec_harvesting.tensix_disable_count is {product_spec.tensix_col_disable_count}, expected {disable_count}"
            )
        _logger.info(
            f"Verified product_spec_harvesting.tensix_disable_count is {product_spec.tensix_col_disable_count}"
        )
        return os.EX_OK

    # Update Tensix disable count
    product_spec = getattr(fw_table, "product_spec_harvesting")
    _logger.debug(
        f"Current product_spec_harvesting.tensix_col_disable_count: {product_spec.tensix_col_disable_count}"
    )
    product_spec.tensix_col_disable_count = disable_count
    _logger.debug(
        f"Updated product_spec_harvesting.tensix_disable_count to {disable_count}"
    )

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
        "disable_count": args.disable_count,
    }

    return fwtable_tooling.do_update(
        args.input,
        args.output,
        args.board,
        set_tensix_disable_count,
        cb_object,
        args.verbose,
    )


if __name__ == "__main__":
    sys.exit(main())
