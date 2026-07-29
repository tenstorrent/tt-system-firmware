#!/usr/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

"""
Flash SPI on Blackhole ASICs over PCIe using SMC messages.

Flashes contents of a provided firmware bundle to the external SPI EEPROM
on Blackhole ASICs over PCIe using SMC messages. By default the script operates
on all ASICs of a board. Use ``--asic-index`` to target a single ASIC
(e.g. on p300 boards which have two).
"""

from __future__ import annotations

import argparse
import base64
import io
import logging
import os
import sys
import tarfile
import tempfile
from intelhex import IntelHex
from pathlib import Path
from typing import Any

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pcie_utils
import pyluwen
import pyocd_utils
import tt_boot_fs

logger = logging.getLogger(Path(__file__).stem)

# Matches SPI sector size in lib/tenstorrent/bh_arc/spi_eeprom.c
DEFAULT_CHUNK_BYTES = 4096

WAIT_FOR_ENUM_S = 120


# ---------------------------------------------------------------------------
# Firmware bundle input
# ---------------------------------------------------------------------------


def _extract_tar_safe(tar: tarfile.TarFile, dest: str) -> None:
    tar.extractall(path=dest, filter="data")


def _bundle_asic_subdir(name: str) -> str:
    """Reject path traversal in ASIC directory names from board metadata."""
    if not name or name in (".", ".."):
        raise ValueError(f"Invalid ASIC name: {name!r}")
    return name


def _read_bootfs_raw_from_b16(image_path: str | Path) -> bytes:
    """Decode ``image.bin`` (base16 lines + optional ``@offset``) to raw bytes."""
    data = bytearray()
    with open(image_path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("@"):
                offset = int(line[1:], 10)
                if offset > len(data):
                    data.extend(b"\xff" * (offset - len(data)))
            else:
                data.extend(base64.b16decode(line))
    return bytes(data)


# ---------------------------------------------------------------------------
# PCIe + Blackhole handle
# ---------------------------------------------------------------------------


def _open_all_blackholes(asic_indices: list[int]) -> dict[int, Any]:
    """Ensure every ASIC is enumerated, then return ``{idx: PciBlackhole}``."""
    for idx in asic_indices:
        if not pcie_utils.wait_for_enum(asic_id=idx, timeout=WAIT_FOR_ENUM_S):
            raise RuntimeError(
                f"ASIC {idx} not ready within {WAIT_FOR_ENUM_S}s (enumeration/ping)"
            )

    chips = pyluwen.detect_chips()
    handles = {}
    for idx in asic_indices:
        if idx >= len(chips):
            raise RuntimeError(
                f"detect_chips() found {len(chips)} chip(s); need index {idx}"
            )
        handles[idx] = chips[idx].as_bh()
    return handles


# ---------------------------------------------------------------------------
# SPI programming (Intel HEX → spi_write)
# ---------------------------------------------------------------------------


def _spi_write_intel_hex(bh: Any, ihex_bytes: bytes | str) -> None:
    """Parse Intel HEX and write each range in ``DEFAULT_CHUNK_BYTES`` slices via ``bh.spi_write``."""
    text = ihex_bytes.decode("ascii") if isinstance(ihex_bytes, bytes) else ihex_bytes
    ih = IntelHex()
    ih.loadhex(io.StringIO(text))
    for start, end in ih.segments():
        addr = start
        while addr < end:
            n = min(DEFAULT_CHUNK_BYTES, end - addr)
            payload = bytes(ih.tobinarray(start=addr, size=n))
            bh.spi_write(int(addr), payload)
            addr += n


def flash_fwbundle_to_spi_smc(
    board_name: str,
    fwbundle_path: Path,
    *,
    asic_index: int | None,
) -> None:
    """Extract ``fwbundle_path`` and program each selected ASIC’s image to SPI."""
    meta = pyocd_utils.load_board_metadata()
    asics = pyocd_utils.select_asics(meta, board_name, asic_index)

    if asics is None:
        raise ValueError(
            f"Invalid ASIC selection for board {board_name!r} (asic_index={asic_index})"
        )

    if not fwbundle_path.is_file():
        raise FileNotFoundError(fwbundle_path)

    with tempfile.TemporaryDirectory() as tmp:
        with tarfile.open(fwbundle_path, "r") as tar:
            _extract_tar_safe(tar, tmp)
        root = Path(tmp).resolve()

        # Resolve handles to every ASIC up front. wait_for_enum rescans the
        # PCIe bus, which must happen before any SPI is reprogrammed (see
        # _open_all_blackholes).
        logger.info("Opening PCIe for ASIC(s) %s...", [idx for idx, _ in asics])
        handles = _open_all_blackholes([idx for idx, _ in asics])

        for idx, asic in asics:
            name = _bundle_asic_subdir(str(asic["name"]))
            image_bin = root / name / "image.bin"
            if not image_bin.is_file():
                raise FileNotFoundError(f"Missing {name}/image.bin in {fwbundle_path}")
            image_resolved = image_bin.resolve(strict=True)
            if not image_resolved.is_relative_to(root):
                raise ValueError(
                    f"image.bin is outside of the expected temp directory: {image_resolved}"
                )

            logger.info("Decoding bootfs for ASIC %d (%s)...", idx, name)
            raw = _read_bootfs_raw_from_b16(image_resolved)
            bootfs = tt_boot_fs.BootFs.from_binary(raw)
            ihex = bootfs.to_intel_hex(True)

            logger.info(
                "Programming SPI via spi_write (%d-byte slices)...",
                DEFAULT_CHUNK_BYTES,
            )
            _spi_write_intel_hex(handles[idx], ihex)
            logger.info("ASIC %d done.", idx)


def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Flash SPI over PCIe using SMC messages.",
        allow_abbrev=False,
    )
    p.add_argument(
        "--board-name",
        required=True,
        help="Board name as listed in board_metadata.yaml (e.g. p100a, p150a, p300a)",
    )
    p.add_argument(
        "--asic-index",
        type=int,
        default=None,
        help="Operate on a single ASIC by index (0-based). "
        "If omitted, all ASICs on the board are targeted.",
    )
    pyocd_utils.add_common_args(p, adapter_id=False, no_prompt=False)

    sub = p.add_subparsers(dest="command", required=True)
    flash = sub.add_parser(
        "flash-fwbundle",
        help="Unpack a .fwbundle and program each ASIC's image to SPI",
    )
    flash.add_argument("file", type=Path, help="Path to .fwbundle")
    return p.parse_args()


def main() -> int:
    args = _parse_args()
    pyocd_utils.setup_logging(args.verbose)

    if args.command != "flash-fwbundle":
        logger.error("No command given")
        return os.EX_USAGE

    try:
        flash_fwbundle_to_spi_smc(
            args.board_name,
            args.file,
            asic_index=args.asic_index,
        )
    except Exception:
        logger.exception("flash-fwbundle failed")
        return os.EX_SOFTWARE

    logger.info("Done.")
    return os.EX_OK


if __name__ == "__main__":
    raise SystemExit(main())
