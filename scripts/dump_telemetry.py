#!/usr/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Dump the Blackhole SMC telemetry table over PCIe using pyluwen.

Follows the MMIO procedure in doc/services/telemetry/index.rst:

1. Read the telemetry_table address from reset_unit.SCRATCH_RAM[13].
2. Read telemetry_table.entry_count.
3. Read all tag_table entries to build the tag -> offset mapping.
4. Read each value from SCRATCH_RAM[12] + 4 * offset.
"""

import argparse
import errno
import re
import sys
from pathlib import Path

import pcie_utils

ARC_RESET_UNIT = 0x80030000
SMC_SCRATCH_RAM_BASE = ARC_RESET_UNIT + 0x400
TELEMETRY_DATA_REG_ADDR = SMC_SCRATCH_RAM_BASE + 12 * 4
TELEMETRY_TABLE_REG_ADDR = SMC_SCRATCH_RAM_BASE + 13 * 4

TABLE_VERSION_OFFSET = 0x0
TABLE_ENTRY_COUNT_OFFSET = 0x4
TABLE_TAG_TABLE_OFFSET = 0x8

# Sanity bound so a garbage table pointer doesn't make us read forever
MAX_ENTRY_COUNT = 1024

TELEMETRY_HEADER = (
    Path(__file__).resolve().parent.parent / "lib/tenstorrent/bh_arc/telemetry.h"
)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Dump the SMC telemetry table over PCIe.", allow_abbrev=False
    )
    parser.add_argument(
        "--asic-id",
        type=int,
        nargs="+",
        default=[0],
        metavar="ID",
        help="One or more ASIC indices to dump telemetry from (default: 0).",
    )
    parser.add_argument(
        "--tags",
        type=str,
        nargs="+",
        metavar="TAG",
        help="Only dump these tags, given as IDs or names (e.g. 11 TAG_AICLK AICLK).",
    )
    return parser.parse_args()


def load_tag_names(header=TELEMETRY_HEADER):
    """Return a {tag_id: name} map parsed from telemetry.h, or {} if unavailable."""
    try:
        text = header.read_text()
    except OSError:
        return {}
    names = {}
    for name, value in re.findall(r"^#define\s+(TAG_\w+)\s+(\d+)\b", text, re.M):
        if name != "TAG_COUNT":
            names[int(value)] = name
    return names


def resolve_tags(tags, tag_names):
    """Convert user-supplied tag IDs / names into a set of tag IDs."""
    by_name = {name: tag for tag, name in tag_names.items()}
    resolved = set()
    for t in tags:
        if t.isdigit() or t.lower().startswith("0x"):
            resolved.add(int(t, 0))
            continue
        name = t.upper()
        if not name.startswith("TAG_"):
            name = "TAG_" + name
        if name not in by_name:
            raise ValueError(f"Unknown telemetry tag: {t}")
        resolved.add(by_name[name])
    return resolved


def read_tag_table(chip):
    """Return (version, data_addr, [(tag, offset), ...]) from the telemetry table."""
    table_addr = chip.axi_read32(TELEMETRY_TABLE_REG_ADDR)
    data_addr = chip.axi_read32(TELEMETRY_DATA_REG_ADDR)
    if table_addr in (0, 0xFFFFFFFF) or data_addr in (0, 0xFFFFFFFF):
        raise RuntimeError(
            f"Telemetry not initialized (table: 0x{table_addr:08x}, "
            f"data: 0x{data_addr:08x})"
        )

    version = chip.axi_read32(table_addr + TABLE_VERSION_OFFSET)
    entry_count = chip.axi_read32(table_addr + TABLE_ENTRY_COUNT_OFFSET)
    if entry_count > MAX_ENTRY_COUNT:
        raise RuntimeError(f"Implausible telemetry entry_count: {entry_count}")

    entries = []
    for i in range(entry_count):
        # struct telemetry_entry { uint16_t tag; uint16_t offset; }, little-endian
        raw = chip.axi_read32(table_addr + TABLE_TAG_TABLE_OFFSET + i * 4)
        entries.append((raw & 0xFFFF, (raw >> 16) & 0xFFFF))
    return version, table_addr, data_addr, entries


def dump_telemetry(asic_id, tags=None, tag_names=None):
    """
    Dump telemetry from one or more ASICs.

    asic_id may be a single int or an iterable of ints. tags, if given, is a set
    of tag IDs to restrict the output to.
    """
    if tag_names is None:
        tag_names = load_tag_names()
    asic_ids = [asic_id] if isinstance(asic_id, int) else list(asic_id)
    rc = 0
    for aid in asic_ids:
        print(f"--- ASIC {aid} ---")
        try:
            chip = pcie_utils.get_chip(aid)
            version, table_addr, data_addr, entries = read_tag_table(chip)
        except Exception as e:
            print(f"Error reading telemetry from ASIC {aid}: {e}")
            rc = errno.EIO
            continue

        print(f"Telemetry table: 0x{table_addr:08x} (version 0x{version:08x})")
        print(f"Telemetry data:  0x{data_addr:08x}")
        print(f"Entry count:     {len(entries)}\n")
        print(f"{'Tag':>4}  {'Name':<30} {'Offset':>6}  {'Hex':>10}  {'Decimal':>11}")

        for tag, offset in sorted(entries):
            if tags and tag not in tags:
                continue
            value = chip.axi_read32(data_addr + 4 * offset)
            name = tag_names.get(tag, f"TAG_{tag}")
            print(f"{tag:>4}  {name:<30} {offset:>6}  0x{value:08x}  {value:>11}")
        print()
    return rc


def main():
    args = parse_args()
    tag_names = load_tag_names()
    tags = None
    if args.tags:
        try:
            tags = resolve_tags(args.tags, tag_names)
        except ValueError as e:
            print(e, file=sys.stderr)
            return errno.EINVAL
    return dump_telemetry(args.asic_id, tags=tags, tag_names=tag_names)


if __name__ == "__main__":
    sys.exit(main())
