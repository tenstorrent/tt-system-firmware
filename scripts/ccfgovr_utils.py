# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Host-side helpers for the ccfgovr A/B override banks

Each bank is one 4 KiB SPI partition (`ccfgovra`, `ccfgovrb`) holding a
20-byte header followed by a null-terminated FwTableOverride protobuf body,
zero padded to 4 bytes:

    magic | seq | body_len | version | cksum | body[body_len]

cksum is CRC32 (IEEE) over the first 16 header bytes and the body. At boot
the firmware takes the plausible bank with the newest seq, falls back to the
other one if its CRC or decode fails, and reports the outcome in telemetry
(TAG_CCFGOVR_STATE, TAG_CCFGOVR_SEQ), see `Status`.

This module encodes the protobuf by hand so tests can also build bodies that
bh-mod refuses to write, such as out-of-range values.
"""

import json
import logging
import os
import shutil
import struct
import subprocess
import time
import zlib
from dataclasses import dataclass

import pyluwen

from dal_utils import ARC_TILE, noc_read32, open_chip

logger = logging.getLogger(__name__)

MAGIC = 0x564F4343
SEQ_ERASED = 0xFFFFFFFF
HEADER_VERSION = 0
HEADER_SIZE = 20
BANK_SIZE = 0x1000
BODY_MAX = 512
BANK_TAGS = ("ccfgovra", "ccfgovrb")

# Per-bank states in TAG_CCFGOVR_STATE, see BH_FWTABLE_CCFGOVR_BANK_* in
# include/zephyr/drivers/misc/bh_fwtable.h
BANK_EMPTY = 0
BANK_STANDBY = 1
BANK_REJECTED = 2
BANK_APPLIED = 3
STATE_HAS_OVERRIDES = 1 << 4

TAG_CCFGOVR_STATE = 81
TAG_CCFGOVR_SEQ = 82

# bh-mod path -> (FwTableOverride field, submessage field) from
# fw_table_override.proto. Every leaf is a varint.
FIELDS = {
    "chip_limits.tdp_limit": (2, 5),
    "chip_limits.max_tdp_limit": (2, 16),
    "chip_limits.kernel_throttler_stop_nops_freq": (2, 17),
    "feature_enable.kernel_throttler_at_floor_en": (3, 10),
    "feature_enable.gddr_therm_trip_en": (3, 11),
    "feature_enable.bypass_cable_fault_detection": (3, 12),
    "dram_table.soft_harvest_dram_mask": (5, 3),
    "pci0_property_table.max_pcie_speed": (7, 1),
    "pci1_property_table.max_pcie_speed": (8, 1),
    "eth_property_table.eth_speed_override": (9, 3),
}

BOOT_STATUS0 = 0x80030408
TELEMETRY_DATA_REG_ADDR = 0x80030430
TELEMETRY_TABLE_REG_ADDR = 0x80030434
FW_ID_SMC_NORMAL = 0
FW_ID_SMC_RECOVERY = 1


def _varint(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def frame(proto):
    """Null-terminate and zero-pad a protobuf message the way bh-mod does"""
    if not proto:
        return b""
    body = proto + b"\0"
    return body + b"\0" * (-len(body) % 4)


def encode_body(overrides, extra=b""):
    """
    Encode {bh-mod path: value} as a framed bank body. extra is appended to
    the top-level message unchanged, for fields FIELDS does not describe
    """
    groups = {}
    for path, value in overrides.items():
        top, leaf = FIELDS[path]
        groups.setdefault(top, bytearray()).extend(
            _varint(leaf << 3) + _varint(int(value))
        )
    proto = bytearray()
    for top in sorted(groups):
        sub = bytes(groups[top])
        proto += _varint((top << 3) | 2) + _varint(len(sub)) + sub
    return frame(bytes(proto) + extra)


def make_bank(
    seq, body=b"", magic=MAGIC, version=HEADER_VERSION, body_len=None, cksum=None
):
    """Build header + body bytes. Leave cksum as None to compute it"""
    body_len = len(body) if body_len is None else body_len
    head = struct.pack("<4I", magic, seq, body_len, version)
    if cksum is None:
        cksum = zlib.crc32(head + body)
    return head + struct.pack("<I", cksum) + body


_STATE_NAMES = ("empty", "standby", "rejected", "applied")


@dataclass(frozen=True)
class Status:
    """The firmware's boot-time bank choice, from TAG_CCFGOVR_STATE and TAG_CCFGOVR_SEQ"""

    state: int
    seq: int

    def bank_state(self, bank):
        """BANK_* state of bank 0 (A) or 1 (B)"""
        return (self.state >> (2 * bank)) & 3

    @property
    def bank(self):
        """The applied bank, 0 for A and 1 for B, or None"""
        return next((b for b in (0, 1) if self.bank_state(b) == BANK_APPLIED), None)

    @property
    def has_overrides(self):
        return bool(self.state & STATE_HAS_OVERRIDES)

    def __str__(self):
        return (
            f"A={_STATE_NAMES[self.bank_state(0)]} B={_STATE_NAMES[self.bank_state(1)]} "
            f"has_overrides={self.has_overrides} seq={self.seq}"
        )


def _open_bh(asic_id):
    return pyluwen.PciChip(pci_interface=asic_id).as_bh()


def bank_addrs(asic_id):
    chip = _open_bh(asic_id)
    try:
        return [chip.get_spirom_table_spi_addr(tag) for tag in BANK_TAGS]
    finally:
        del chip


def read_spi(asic_id, addr, size):
    chip = _open_bh(asic_id)
    try:
        buf = bytearray(size)
        chip.spi_read(addr, buf)
        return bytes(buf)
    finally:
        del chip


def write_spi(asic_id, addr, data):
    """Write data at addr. The firmware merges partial sector writes"""
    chip = _open_bh(asic_id)
    try:
        chip.spi_write(addr, bytes(data))
    finally:
        del chip


def read_banks(asic_id):
    """Raw header and body bytes of both banks, in (A, B) order"""
    chip = _open_bh(asic_id)
    try:
        banks = []
        for tag in BANK_TAGS:
            buf = bytearray(HEADER_SIZE + BODY_MAX)
            chip.spi_read(chip.get_spirom_table_spi_addr(tag), buf)
            banks.append(bytes(buf))
        return banks
    finally:
        del chip


BH_MOD = shutil.which("bh-mod") or os.path.expanduser("~/bh-mod")


def have_bh_mod():
    return os.path.isfile(BH_MOD)


def run_bh_mod(asic_id, *args):
    """Run bh-mod on /dev/tenstorrent/<asic_id>, or on every chip for None"""
    dev = () if asic_id is None else ("-d", f"/dev/tenstorrent/{asic_id}")
    return subprocess.run(
        [BH_MOD, *dev, *args], capture_output=True, text=True, check=False
    )


# Cleared once a per-chip reset fails on a Galaxy, so the rest of the run goes
# straight to -glx_reset
_galaxy_chip_reset = True


def smi_reset(asic_id, board_name):
    """
    Reset /dev/tenstorrent/<asic_id> with tt-smi, or every chip for None,
    returning its exit code. A Galaxy only takes a per-chip tt-smi -r from
    CPLD v1.16, so there it falls back to resetting every tray, which is
    also how every chip on a Galaxy is reset
    """
    global _galaxy_chip_reset

    galaxy = board_name is not None and "galaxy" in board_name.lower()
    target = () if asic_id is None else (f"/dev/tenstorrent/{asic_id}",)
    if not galaxy or (_galaxy_chip_reset and asic_id is not None):
        result = subprocess.run(
            ["tt-smi", "-r", *target, "--eth_train_skip"],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode == 0 or not galaxy:
            return result.returncode
        logger.warning(
            "Per-chip tt-smi -r failed on this Galaxy (CPLD older than v1.16?), "
            "using -glx_reset from now on: %s",
            result.stdout.strip()[-500:],
        )
        _galaxy_chip_reset = False
    cmd = ["tt-smi", "-glx_reset", "--eth_train_skip"]
    return subprocess.run(cmd, capture_output=True, check=False).returncode


def bh_mod_write(asic_id, board_name, *args):
    """
    Run a bh-mod write, then reset the chip with tt-smi so it takes effect.
    Returns the exit code of whichever command failed, else 0
    """
    rc = run_bh_mod(asic_id, *args).returncode
    if rc != 0:
        return rc
    return smi_reset(asic_id, board_name)


def bh_mod_get(asic_id, *paths, delta=False):
    """
    Return the fw-table view of /dev/tenstorrent/<asic_id> as
    {path: {"default": ..., "override": ...}}. Raises RuntimeError if bh-mod
    fails
    """
    args = ["get", "-f", "json", *(["--delta"] if delta else []), *paths]
    result = run_bh_mod(asic_id, *args)
    if result.returncode != 0:
        raise RuntimeError(f"bh-mod {' '.join(args)} failed: {result.stderr.strip()}")
    return json.loads(result.stdout)


def active_overrides(asic_id):
    """{path: value} for every field the applied bank overrides"""
    return {
        path: row["override"] for path, row in bh_mod_get(asic_id, delta=True).items()
    }


def read_telem_tags(asic_id, *tags):
    """
    {tag: value} for the given telemetry tags, looked up through the tag
    table, so a tag the running firmware does not report maps to None
    """
    with open_chip(asic_id) as (_, tlb):
        table = noc_read32(tlb, *ARC_TILE, TELEMETRY_TABLE_REG_ADDR)
        data = noc_read32(tlb, *ARC_TILE, TELEMETRY_DATA_REG_ADDR)
        count = noc_read32(tlb, *ARC_TILE, table + 4)
        offsets = {}
        # struct telemetry_entry {uint16_t tag; uint16_t offset;} from table + 8
        for i in range(min(count, 0x100)):
            entry = noc_read32(tlb, *ARC_TILE, table + 8 + 4 * i)
            offsets[entry & 0xFFFF] = entry >> 16
        return {
            tag: noc_read32(tlb, *ARC_TILE, data + 4 * offsets[tag])
            if tag in offsets
            else None
            for tag in tags
        }


def status(asic_id):
    """The firmware's bank choice at its last boot, or None if it does not report one"""
    tags = read_telem_tags(asic_id, TAG_CCFGOVR_STATE, TAG_CCFGOVR_SEQ)
    if None in tags.values():
        return None
    return Status(tags[TAG_CCFGOVR_STATE], tags[TAG_CCFGOVR_SEQ])


def leftover_overrides(asic_id):
    """
    Describe any override set on the card, or return "" if there is none.
    Both the firmware's report of the bank it booted with and the bank
    bh-mod reads on SPI are checked, so an override written but not booted
    yet, or one bh-mod does not show, is still found
    """
    found = []
    booted = status(asic_id)
    if booted is not None and booted.has_overrides:
        found.append(f"booted with overrides ({booted})")
    on_spi = active_overrides(asic_id)
    if on_spi:
        found.append(f"on SPI: {on_spi}")
    return "; ".join(found)


def boot_status(asic_id):
    """Read BOOT_STATUS0 over the NOC, or None if the chip can't be read"""
    try:
        with open_chip(asic_id) as (_, tlb):
            return noc_read32(tlb, *ARC_TILE, BOOT_STATUS0)
    except OSError:
        return None


def wait_fw_ready(asic_id, fw_id, timeout=60):
    """
    Wait until the firmware identified by fw_id has finished init and its
    message queue is up. Unlike dal_utils.wait_arc_boot this also works for
    the recovery image, which does not post the main firmware's postcodes
    """
    deadline = time.monotonic() + timeout
    status = None
    while time.monotonic() < deadline:
        status = boot_status(asic_id)
        # msg_queue_ready (bit 0), hw_init_status (bits 2:1) done or error,
        # fw_id (bits 6:3)
        if (
            status is not None
            and status & 1
            and (status >> 1) & 3 >= 2
            and (status >> 3) & 0xF == fw_id
        ):
            return status
        time.sleep(0.5)
    raise TimeoutError(
        f"fw_id {fw_id} not ready within {timeout}s (BOOT_STATUS0 {status})"
    )
