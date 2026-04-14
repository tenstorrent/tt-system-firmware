#!/usr/bin/env python3

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import base64
import ctypes
from dataclasses import dataclass
import io
import logging
import os
from pathlib import Path
import pykwalify.core
import struct
from typing import Any, Callable, cast, Iterable, Optional, Tuple
import yaml
import argparse
import sys
import json
import tempfile
from base64 import b16encode
from intelhex import IntelHex
import imgtool.image as imgtool_image

try:
    from yaml import CSafeLoader as SafeLoader
except ImportError:
    from yaml import SafeLoader

# Define constants
FD_HEAD_ADDR = 0x0
SECURITY_BINARY_FD_ADDR = 0x3FE0
SPI_RX_ADDR = 0x13FFC
SPI_RX_VALUE = 0xA5A55A5A
SPI_RX_SIZE = 4
FAILOVER_HEAD_ADDR = 0x4000
MAX_TAG_LEN = 8
FD_SIZE = 32
CKSUM_SIZE = 4
IMAGE_ADDR = 0x14000

SCHEMA_PATH = (
    Path(__file__).parents[1] / "scripts" / "schemas" / "tt-boot-fs-schema.yml"
)

ROOT = Path(__file__).parents[1]

_logger = logging.getLogger(__name__)


class ExtendedStructure(ctypes.Structure):
    def __eq__(self, other):
        if not isinstance(other, self.__class__):
            return False
        for field in self._fields_:
            field_name = field[0]

            self_value = getattr(self, field_name)
            other_value = getattr(other, field_name)

            # Handle comparison for ctypes.Array fields
            if isinstance(self_value, ctypes.Array):
                if len(self_value) != len(other_value):
                    return False
                for i, _ in enumerate(self_value):
                    if self_value[i] != other_value[i]:
                        return False
            else:
                if self_value != other_value:
                    return False
        return True

    def __ne__(self, other):
        return self != other

    def __repr__(self):
        field_strings = []
        for field in self._fields_:
            field_name = field[0]

            field_value = getattr(self, field_name)

            # Handle string representation for ctypes.Array fields
            if field_name in {"copy_dest", "data_crc", "fd_crc", "spi_addr"}:
                field_strings.append(f"{field_name}=0x{field_value:x}")
            elif field_name in {"image_tag"}:
                tag = "".join(("" if x == 0 else chr(x)) for x in field_value)
                field_strings.append(f'{field_name}="{tag}"')
            elif isinstance(field_value, ctypes.Array):
                array_str = ", ".join(str(x) for x in field_value)
                field_strings.append(f"{field_name}=[{array_str}]")
            else:
                field_strings.append(f"{field_name}={field_value}")

        fields_repr = ", ".join(field_strings)
        return f"{self.__class__.__name__}({fields_repr})"


class ExtendedUnion(ctypes.Union):
    def __eq__(self, other):
        for fld in self._fields_:
            if getattr(self, fld[0]) != getattr(other, fld[0]):
                return False
        return True

    def __ne__(self, other):
        for fld in self._fields_:
            if getattr(self, fld[0]) != getattr(other, fld[0]):
                return True
        return False

    def __repr__(self):
        field_strings = []
        for field in self._fields_:
            field_name = field[0]

            field_value = getattr(self, field_name)
            field_strings.append(f"{field_name}={field_value}")
        fields_repr = ", ".join(field_strings)
        return f"{self.__class__.__name__}({fields_repr})"


# Define fd_flags structure
class fd_flags(ExtendedStructure):
    _fields_ = [
        ("image_size", ctypes.c_uint32, 24),
        ("invalid", ctypes.c_uint32, 1),
        ("executable", ctypes.c_uint32, 1),
        ("fd_flags_rsvd", ctypes.c_uint32, 6),
    ]


# Define fd_flags union
class fd_flags_u(ExtendedUnion):
    _fields_ = [("val", ctypes.c_uint32), ("f", fd_flags)]


# Define security_fd_flags structure
class security_fd_flags(ExtendedStructure):
    _fields_ = [
        ("signature_size", ctypes.c_uint32, 12),
        ("sb_phase", ctypes.c_uint32, 8),  # 0 - Phase0A, 1 - Phase0B
    ]


# Define security_fd_flags union
class security_fd_flags_u(ExtendedUnion):
    _fields_ = [("val", ctypes.c_uint32), ("f", security_fd_flags)]


# Define tt_boot_fs_fd structure (File descriptor)
class tt_boot_fs_fd(ExtendedStructure):
    _fields_ = [
        ("spi_addr", ctypes.c_uint32),
        ("copy_dest", ctypes.c_uint32),
        ("flags", fd_flags_u),
        ("data_crc", ctypes.c_uint32),
        ("security_flags", security_fd_flags_u),
        ("image_tag", ctypes.c_uint8 * MAX_TAG_LEN),
        ("fd_crc", ctypes.c_uint32),
    ]

    def image_tag_str(self):
        output = ""
        for c in self.image_tag:
            if c == 0:
                break
            output += chr(c)
        return output


def read_fd(reader, addr: int) -> tt_boot_fs_fd:
    fd = reader(addr, ctypes.sizeof(tt_boot_fs_fd))
    return tt_boot_fs_fd.from_buffer_copy(fd)


def iter_fd(reader: Callable[[int, int], bytes]):
    curr_addr = 0
    while True:
        fd = read_fd(reader, curr_addr)

        if fd.flags.f.invalid != 0:
            return None

        yield curr_addr, fd

        curr_addr += ctypes.sizeof(tt_boot_fs_fd)


def read_tag(
    reader: Callable[[int, int], bytes], tag: str
) -> Optional[Tuple[int, tt_boot_fs_fd]]:
    for addr, fd in iter_fd(reader):
        if fd.image_tag_str() == tag:
            return addr, fd


@dataclass
class FsEntry:
    provisioning_only: bool

    tag: str
    data: bytes
    spi_addr: int
    load_addr: int
    executable: bool

    def get_descriptor(self) -> tt_boot_fs_fd:
        image_tag = [0] * MAX_TAG_LEN
        for index, c in enumerate(self.tag):
            image_tag[index] = ord(c)

        if self.spi_addr is None:
            self.spi_addr = 0

        if self.load_addr is None:
            self.load_addr = 0

        fd = tt_boot_fs_fd(
            spi_addr=self.spi_addr,
            copy_dest=self.load_addr,
            image_tag=(ctypes.c_uint8 * MAX_TAG_LEN)(*image_tag),
            data_crc=cksum(self.data),
            flags=fd_flags_u(
                f=fd_flags(
                    image_size=len(self.data),
                    executable=self.executable,
                    invalid=0,
                )
            ),
        )
        fd.fd_crc = cksum(bytes(fd))
        return fd

    def descriptor(self) -> bytes:
        return bytes(self.get_descriptor())


@dataclass
class FileAlignment:
    flash_size: int
    block_size: int

    @staticmethod
    def loads(data: dict[str, Any]):
        return FileAlignment(
            flash_size=data["flash_device_size"], block_size=data["flash_block_size"]
        )


@dataclass
class BootImage:
    provisioning_only: bool

    tag: str
    binary: bytes
    executable: bool
    spi_addr: Optional[int]
    load_addr: int

    @staticmethod
    def _resolve_environment_variables(value: str, env: dict):
        # Replace value with any environment settings
        for k, v in env.items():
            value = value.replace(k, v)

        return value

    # To handle putting an image at the end of the firmware I use this simple eval to handle the expression
    # which performs this placement.
    @staticmethod
    def _eval_firmware_address(source: Any, alignment: FileAlignment) -> Any:
        source = str(source)
        # $END is a special variable that refers to the end of the flash region
        source = source.replace("$END", str(alignment.flash_size))

        # do a simple python eval to handle any expressions left over
        return eval(source)

    @staticmethod
    def loads(tag: str, data: dict[str, Any], alignment: FileAlignment, env: dict):
        expanded_path = BootImage._resolve_environment_variables(data["binary"], env)
        if not os.path.isfile(expanded_path):
            raise ValueError(f"path {expanded_path} is not a file")

        binary = open(expanded_path, "rb").read()
        if data["padto"] != 0:
            padto = data["padto"]
            if padto % 4 != 0:
                raise ValueError(f"{tag} padto value {padto} is not a multiple of 4")
            if padto < len(binary):
                raise ValueError(
                    f"{tag} padto value {padto} is < the binary size {len(binary)}"
                )
            binary += bytes(padto - len(binary))
        # We always need to pad binaries to 4 byte offsets for checksum verification
        binary += bytes((len(binary) % 4))

        if len(tag) > MAX_TAG_LEN:
            raise ValueError(f"{tag} is longer than the maximum allowed tag size (8).")

        executable = data.get("executable", False)
        load_addr = data.get("offset")

        if executable and load_addr is None:
            raise ValueError(
                f"While loading {tag} If executable is set load_addr must also be set"
            )

        if load_addr is None:
            load_addr = 0

        return BootImage(
            provisioning_only=data.get("provisioning_only", False),
            tag=tag,
            binary=binary,
            executable=executable,
            spi_addr=BootImage._eval_firmware_address(data.get("source"), alignment),
            load_addr=load_addr,
        )


class RangeTracker:
    def __init__(self, alignment: int) -> None:
        self.ranges: list[tuple[int, int, Optional[Any]]] = []
        self.alignment = alignment

    def add(self, start: int, end: int, data: Optional[Any]):
        # Not trying to be clever...
        # This would be optimially solved with some type of tree, but that's not
        # needed for the number of entries that we are dealing with
        insert_index = 0
        for index, range in enumerate(self.ranges):
            if (range[0] <= start < range[1]) or (range[0] < end <= range[1]):
                # Overlap! Raise Error
                raise Exception(
                    f"Range {start:x}:{end:x} overlaps with existing range {range[0]}:{range[1]}"
                )
            elif range[0] > start:
                # Range not found...
                # I know self.ranges is in order so can stop looking here
                insert_index = index
                break
        else:
            insert_index = len(self.ranges)
        # Sanity... make sure we are aligned to the alignment value
        if start % self.alignment != 0:
            raise ValueError(
                f"The range {start:x}:{end:x} is not aligned to {self.alignment}"
            )
        self.ranges.insert(insert_index, (start, end, data))

    def _align_up(self, value: int) -> int:
        return (value + self.alignment - 1) & ~(self.alignment - 1)

    def find_gap_of_size(self, size: int) -> tuple[int, int]:
        if len(self.ranges) == 0:
            return (0, size)

        # If the first start is > 0 check if we can stick outselves there
        if self.ranges[0][0] > size:
            return (0, size)

        last_end = self._align_up(self.ranges[0][1])
        for range in self.ranges[1:]:
            if range[0] - last_end >= size:
                break
            last_end = self._align_up(range[1])

        return (last_end, last_end + size)

    def insert(self, size: int, data: Any):
        (start, end) = self.find_gap_of_size(size)
        self.add(start, end, data)

    def iter(self) -> Iterable[tuple[int, Any]]:
        return iter(
            map(lambda x: (x[0], x[2]), filter(lambda x: x[2] is not None, self.ranges))
        )


class BootFs:
    def __init__(
        self, order: list[str], entries: dict[str, FsEntry], failover: FsEntry
    ) -> None:
        self.order = order
        self.entries = entries
        self.entries["failover"] = failover

    def writes(self) -> list[tuple[bool, int, bytes]]:
        # Write image descriptors and data
        descriptor_addr = 0
        writes: list[tuple[bool, int, bytes]] = []
        for tag in self.order:
            entry = self.entries[tag]
            descriptor = entry.descriptor()
            writes.append((True, descriptor_addr, descriptor))
            writes.append((not entry.provisioning_only, entry.spi_addr, entry.data))
            descriptor_addr += len(descriptor)

        # Handle failover
        writes.append((True, FAILOVER_HEAD_ADDR, self.entries["failover"].descriptor()))
        writes.append(
            (True, self.entries["failover"].spi_addr, self.entries["failover"].data)
        )

        # Handle RTR training value
        writes.append((True, SPI_RX_ADDR, (SPI_RX_VALUE).to_bytes(4, "little")))

        writes.sort(key=lambda x: x[1])

        # check_overlap
        tracker = RangeTracker(1)
        for write in writes:
            tracker.add(write[1], write[1] + len(write[2]), None)

        return writes

    def to_binary(self, all_sections) -> bytes:
        write = bytearray()
        last_addr = 0
        for always_write, addr, data in self.writes():
            if not (always_write or all_sections):
                continue
            write.extend([0xFF] * (addr - last_addr))
            write.extend(data)
            last_addr = addr + len(data)
        return bytes(write)

    def to_intel_hex(self, all_sections) -> bytes:
        output = ""
        current_segment = -1  # Track the current 16-bit segment

        for always_write, address, data in self.writes():
            if not (always_write or all_sections):
                continue
            end_address = address + len(data)

            # Process data in chunks that stay within segment boundaries
            while address < end_address:
                # Calculate the segment and offset
                segment = (address >> 16) & 0xFFFF
                offset = address & 0xFFFF

                next_segment = (segment + 1) << 16

                if address & ~0xFFFF_FFFF != 0:
                    raise Exception(
                        "FW is being written to an address past 4G, cannot represent with ihex!"
                    )

                # If the segment changes, emit an Extended Segment Address
                # Record
                if segment != current_segment:
                    current_segment = segment

                    record_length = "02"
                    load_offset = "0000"
                    record_type = "04"
                    segment_bytes = segment.to_bytes(2, "big")

                    record = f"{record_length}{load_offset}{record_type}{segment_bytes.hex()}"

                    checksum = 0
                    for i in range(0, len(record), 2):
                        hex_byte = int(record[i:][:2], 16)
                        checksum += hex_byte
                    checksum = (-checksum) & 0xFF

                    output += f":{record}{checksum:02X}\n"

                # Calculate how much data to write within this segment (up to
                # 16 bytes)
                segment_end = min(address + 16, end_address, next_segment)
                chunk_size = segment_end - address

                chunk = data[:chunk_size]
                data = data[chunk_size:]
                byte_count = len(chunk)

                # Create the data record
                record_address = offset
                record_type = "00"  # Data record
                data_hex = chunk.hex().upper()

                record = f"{byte_count:02X}{record_address:04X}{record_type}{data_hex}"

                checksum = 0
                for i in range(0, len(record), 2):
                    hex_byte = int(record[i:][:2], 16)
                    checksum += hex_byte
                checksum = (-checksum) & 0xFF

                # Build the record line
                output += f":{record}{checksum:02X}\n"

                # Update start address for next chunk
                address += chunk_size

        # Add end-of-file record
        output += ":00000001FF"
        return output.encode("ascii")

    @staticmethod
    def check_entry(
        tag: str, fd: tt_boot_fs_fd, data: bytes, alignment: int = 0x1000
    ) -> FsEntry:
        data_offs = fd.spi_addr
        if data_offs % alignment != 0:
            raise ValueError(f"{tag} image not aligned to 0x{alignment:x}")

        image_size = fd.flags.f.image_size
        required_size = image_size + CKSUM_SIZE
        if len(data) < required_size:
            raise ValueError(
                f"data len {len(data)} is too small to contain image '{tag}'"
            )
        image_data = data[data_offs : data_offs + image_size]
        data_offs += image_size
        actual_image_cksum = cksum(image_data)

        expected_image_cksum = fd.data_crc
        if expected_image_cksum != actual_image_cksum:
            if tag == "boardcfg":
                # currently, the boardcfg checksum does not seem to be added correctly in images ignore for now.
                pass
            else:
                raise ValueError(
                    f"{tag} image checksum 0x{actual_image_cksum:08x} does not match expected checksum 0x{expected_image_cksum:08x}"
                )

        return FsEntry(
            provisioning_only=False,
            # do not use fd.image_tag_str() as it may be blank for e.g. "failover"
            tag=tag,
            data=image_data,
            spi_addr=fd.spi_addr,
            load_addr=fd.copy_dest,
            executable=fd.flags.f.executable,
        )

    @staticmethod
    def from_binary(data: bytes, alignment: int = 0x1000) -> BootFs:
        data_offs = 0
        order: list[str] = []
        entries: dict[str, FsEntry] = {}
        fds: dict[str, tt_boot_fs_fd] = {}
        failover: FsEntry = None
        failover_fd: tt_boot_fs_fd = None

        # scan fds at the start of the binary
        for value in iter_fd(lambda addr, size: data[addr : addr + size]):
            tag = value[1].image_tag_str()
            fds[tag] = value[1]
            order.append(tag)
        data_offs += FD_SIZE * len(fds)

        if len(data) < FAILOVER_HEAD_ADDR + FD_SIZE:
            raise ValueError(
                f"recovery descriptor not found at fixed offset 0x{FAILOVER_HEAD_ADDR:x}"
            )

        failover_fd = read_fd(
            lambda addr, size: data[addr : addr + size], FAILOVER_HEAD_ADDR
        )

        if len(data) < SPI_RX_ADDR + SPI_RX_SIZE:
            raise ValueError(
                f"data length {len(data)} does not include spi rx training at 0x{SPI_RX_ADDR:x}"
            )

        data_offs = SPI_RX_ADDR

        spi_rx_training = struct.unpack_from("<I", data, data_offs)[0]
        if spi_rx_training != SPI_RX_VALUE:
            raise ValueError(f"spi rx training data not found at 0x{SPI_RX_ADDR:x}")

        for tag in order:
            entries[tag] = BootFs.check_entry(tag, fds[tag], data, alignment)
        failover = BootFs.check_entry("failover", failover_fd, data, alignment)

        return BootFs(order, entries, failover)

    def to_b16(self) -> str:
        ih = IntelHex()
        ih.loadhex(io.StringIO(self.to_intel_hex(True).decode("ascii")))
        b16out = ""
        for off, end in ih.segments():
            b16out += f"@{off}\n"
            b16out += b16encode(ih.tobinarray(start=off, size=end - off)).decode(
                "ascii"
            )
            b16out += "\n"

        return b16out


@dataclass
class FileImage:
    name: str
    product_name: str
    gen_name: str

    alignment: FileAlignment

    images: list[BootImage]
    failover: BootImage

    @staticmethod
    def load(path: str, env: dict):
        try:
            schema = yaml.load(open(SCHEMA_PATH, "r"), Loader=SafeLoader)
            data = yaml.load(open(path, "r"), Loader=SafeLoader)
            data = pykwalify.core.Core(source_data=data, schema_data=schema).validate()
        except Exception as e:
            _logger.error(
                f"Failed to validate {path} against schema {SCHEMA_PATH}: {e}"
            )
            return None

        alignment = FileAlignment.loads(data["alignment"])
        images = {}
        for ent in data["images"]:
            ent_name = ent["name"]
            if ent_name in images:
                raise ValueError(f"Found duplicate image name '{ent_name}'")
            images[ent_name] = BootImage.loads(ent_name, ent, alignment, env)

        return FileImage(
            name=data["name"],
            product_name=data["product_name"],
            gen_name=data["gen_name"],
            alignment=alignment,
            images=images,
            failover=BootImage.loads("", data["fail_over_image"], alignment, env),
        )

    def to_boot_fs(self):
        # We need to
        # - Load all binaries
        # - Place all binaries that have given addresses at the given locations
        #   - Require that all addresses are aligned to block_size
        # - Place all remaining binaries at next available location
        #   - Available location defined as gap aligned to block_size that is large enough for the binary
        # - Generate boot_fs header based on binary placement
        # - Generate image based on boot_fs
        #   - Leave anything out that is marked provisining only
        # - Generate intelhex based on boot_fs
        #   - For provisining
        tracker = RangeTracker(self.alignment.block_size)

        # Reserve bootrom addresses in the tracker
        # The descriptors themselves will be at 0 -> 0x3fc0
        # initial tRoot image is at 0x3fc0 -> 0x4000
        # fail-over descriptor is at 0x4000 -> 0x4040
        # fail-over image is at the end of all images
        # RTR training value is at 0x13ffc -> 14000
        tracker.add(0, 0x14000, None)

        # Make sure that the binaries with a given spi_addr are properly aligned
        # And add to our range tracker
        for image in self.images.values():
            if image.spi_addr is not None:
                if image.spi_addr % self.alignment.block_size != 0:
                    raise ValueError(
                        f"The spi_addr of {image.tag} at {image.spi_addr:x} "
                        "is not aligned to the spi block size of {self.alignment.block_size}"
                    )
                tracker.add(image.spi_addr, image.spi_addr + len(image.binary), image)

        tag_order: list[str] = []
        for image in self.images.values():
            # Add the rest of the images
            if image.spi_addr is None:
                tracker.insert(len(image.binary), image)

            # We need to make sure that we preserve the order of the executable
            # images in the boot_fs header
            if image.executable:
                tag_order.append(image.tag)

        boot_fs = {}
        for addr, image in tracker.iter():
            image = cast(BootImage, image)
            boot_fs[image.tag] = FsEntry(
                provisioning_only=image.provisioning_only,
                tag=image.tag,
                data=image.binary,
                spi_addr=addr,
                load_addr=image.load_addr,
                executable=image.executable,
            )

            if image.tag not in tag_order:
                tag_order.append(image.tag)

        if self.failover.spi_addr is not None:
            # Respect the "source" value set for the failover image
            failover_spi_addr = self.failover.spi_addr
        else:
            # Find a gap for the failover image
            failover_spi_addr = tracker.find_gap_of_size(len(self.failover.binary))[0]

        return BootFs(
            tag_order,
            boot_fs,
            FsEntry(
                provisioning_only=False,
                tag=self.failover.tag,
                data=self.failover.binary,
                spi_addr=failover_spi_addr,
                load_addr=self.failover.load_addr,
                executable=True,
            ),
        )


def cksum(data: bytes):
    calculated_checksum = 0

    if len(data) < 4:
        return 0

    for i in range(0, len(data), 4):
        value = int.from_bytes(data[i:][:4], "little")
        calculated_checksum += value

    calculated_checksum &= 0xFFFFFFFF

    return calculated_checksum


def mkfs(path: Path, env={"$ROOT": str(ROOT)}, hex=False, all_sections=False) -> bytes:
    fi = None
    try:
        fi = FileImage.load(path, env)
        if hex:
            return fi.to_boot_fs().to_intel_hex(all_sections)
        else:
            return fi.to_boot_fs().to_binary(all_sections)
    except Exception as e:
        _logger.error(f"Exception: {e}")
    return None


def fsck(path: Path, alignment: int = 0x1000) -> bool:
    fs = None
    try:
        if path.suffix == ".hex":
            # Read hex file and convert to binary
            ih = IntelHex(str(path))
            data = ih.tobinarray()
        else:
            data = open(path, "rb").read()
        fs = BootFs.from_binary(data, alignment=alignment)
    except Exception as e:
        _logger.error(f"Exception: {e}")
    return fs is not None


def hexdump(start_addr: int, data: bytes, checksum: bool = False):
    def to_printable_ascii(byte):
        return chr(byte) if 32 <= byte <= 126 else "."

    # to remember chunk state
    nskipped_chunks = 0
    prev_chunk = []

    offset = 0
    while offset < len(data):
        chunk = data[offset : offset + 16]

        # skip if chunk is identical to prevuious
        if chunk == prev_chunk:
            nskipped_chunks += 1
            offset += 16
            continue

        if nskipped_chunks > 0:
            # avoid printing duplicate lines (zero information gain)
            print("...")

        hex_values = " ".join(f"{byte:02x}" for byte in chunk)
        ascii_values = "".join(to_printable_ascii(byte) for byte in chunk)
        print(f"{start_addr + offset:08x}  {hex_values:<48}  |{ascii_values}|")
        offset += 16

        # reset chunk state
        prev_chunk = chunk
        nskipped_chunks = 0

    if checksum:
        print(f"checksum: {cksum(data):08x}")


def ls(
    bootfs: Path, verbose: int = 0, output_json: bool = False, input_base64=False
) -> bool:
    fds = []

    try:
        if input_base64:
            data = bytes(0)
            # Pad with 0x0 between offsets
            with open(bootfs, "r") as f:
                lines = f.readlines()
                for line in lines:
                    if line.startswith("@"):
                        # This is an address line, pad data to this point
                        offset = int(line[1:], 10)
                        data += bytes([0xFF] * (offset - len(data)))  # Pad with 0x0
                    else:
                        # This is a data line, decode and append
                        data += base64.b16decode(line.strip())
        elif bootfs.suffix == ".hex":
            # Read hex file and convert to binary
            ih = IntelHex(str(bootfs))
            data = ih.tobinarray()
        else:
            data = open(bootfs, "rb").read()
        fs = BootFs.from_binary(data)

        if verbose >= 0 and not output_json:
            hdr = "spi_addr\timage_tag\tsize\tcopy_dest\tdata_crc\tflags\t\tfd_crc\t\tdigest"
            print(hdr)
            bar = "-" * len(hdr.expandtabs())
            print(bar)

        order: list[(int, str)] = []
        for tag, entry in fs.entries.items():
            order.append((entry.spi_addr, tag))
        order.sort(key=lambda x: x[0])
        order = list(map(lambda x: x[1], order))

        for tag in order:
            entry = fs.entries[tag]
            fd = entry.get_descriptor()

            img_digest_str = "N/A"
            if len(entry.data) >= 4:
                magic = int.from_bytes(entry.data[:4], "little")
                if magic == imgtool_image.IMAGE_MAGIC:
                    # imgtool methods for verifying image expect a file, so create a temp file
                    with tempfile.NamedTemporaryFile() as temp_file:
                        temp_file.write(entry.data)
                        temp_file.flush()
                        ret, _, digest, _ = imgtool_image.Image.verify(
                            temp_file.name, None
                        )
                        if ret == imgtool_image.VerifyResult.OK:
                            img_digest_str = digest.hex()

            obj = {
                "spi_addr": fd.spi_addr,
                "image_tag": tag,
                "size": len(entry.data),
                "copy_dest": fd.copy_dest,
                "data_crc": fd.data_crc,
                "flags": fd.flags.val,
                "fd_crc": fd.fd_crc,
                "digest": img_digest_str,
            }
            fds.append(obj)

            # if very quiet, then don't even print out data
            if verbose <= -2:
                continue

            if not output_json:
                print(
                    f"{fd.spi_addr:08x}\t{tag:<8}\t{len(entry.data)}\t{fd.copy_dest:08x}\t"
                    f"{fd.data_crc:08x}\t{fd.flags.val:08x}\t{fd.fd_crc:08x}\t{img_digest_str:.10}"
                )

                if verbose >= 2:
                    hexdump(start_addr=fd.spi_addr, data=entry.data)

    except Exception as e:
        # Only log if this is being run as a script
        if __name__ == "__main__":
            _logger.error(f"Exception: {e}")

    if fds and output_json and verbose >= 0:
        print(json.dumps(fds))

    return fds


def extract(bootfs: Path, tag: str, output: Path, input_base64=False):
    try:
        if input_base64:
            data = bytes(0)
            # Pad with 0x0 between offsets
            with open(bootfs, "r") as f:
                lines = f.readlines()
                for line in lines:
                    if line.startswith("@"):
                        # This is an address line, pad data to this point
                        offset = int(line[1:], 10)
                        data += bytes([0xFF] * (offset - len(data)))  # Pad with 0x0
                    else:
                        # This is a data line, decode and append
                        data += base64.b16decode(line.strip())
        elif bootfs.suffix == ".hex":
            # Read hex file and convert to binary
            ih = IntelHex(str(bootfs))
            data = ih.tobinarray()
        else:
            data = open(bootfs, "rb").read()
        fs = BootFs.from_binary(data)

        entry_data = None
        for t, entry in fs.entries.items():
            if t == tag:
                entry_data = entry.data
                break
        if entry_data is None:
            _logger.error(f"Tag {tag} not found")
            return os.EX_DATAERR
        with open(output, "wb") as f:
            f.write(entry_data)
    except Exception as e:
        _logger.error(f"Exception: {e}")


def extract_all(bootfs: Path, input_base64=False):
    if input_base64:
        data = bytes(0)
        # Pad with 0x0 between offsets
        with open(bootfs, "r") as f:
            lines = f.readlines()
            for line in lines:
                if line.startswith("@"):
                    # This is an address line, pad data to this point
                    offset = int(line[1:], 10)
                    # Pad with 0x0
                    data += bytes([0xFF] * (offset - len(data)))
                else:
                    # This is a data line, decode and append
                    data += base64.b16decode(line.strip())
    elif bootfs.suffix == ".hex":
        # Read hex file and convert to binary
        ih = IntelHex(str(bootfs))
        data = ih.tobinarray()
    else:
        data = open(bootfs, "rb").read()

    return data


def _generate_bootfs_yaml(
    args, partitions_node, name: str, gen_name: str, out_yaml: str
):
    """
    Parameters:
      name - Name of the board (eg. P100A-1, P300A-1_left)
      gen_name - Gen name of the board (eg. P100A, P300A_L)
      out_yaml - Output YAML file name
    """

    prod_name = args.board.upper()

    partitions_yml = {
        "name": name,
        "product_name": prod_name,
        "gen_name": gen_name,
        "alignment": {
            "flash_device_size": partitions_node.props["flash-device-size"].val,
            "flash_block_size": partitions_node.props["flash-block-size"].val,
        },
        "images": [],
    }

    _logger.debug(partitions_yml)

    for partition in partitions_node.children.values():
        # Galaxy and galaxy_revc do not have BM firmware
        if args.board.startswith("galaxy") and partition.label == "bmfw":
            continue
        # P300 right chip does not have BM firmware
        if name[-5:] == "right" and partition.label == "bmfw":
            continue
        # P300 does not have origcfg
        if args.board == "p300" and partition.label == "origcfg":
            continue

        label = partition.label
        if "binary-path" not in partition.props:
            continue  # No tt-boot-fs entry for this partition

        path = partition.props["binary-path"].val
        path = path.replace("$PROD_NAME", prod_name)
        path = path.replace("$BOARD_REV", gen_name)
        path = path.replace("$BUILD_DIR", str(args.build_dir))
        path = path.replace("$BLOBS_DIR", str(args.blobs_dir))

        image_entry = {
            "name": label,
            "binary": path,
        }
        # Build image entry based on which fields the partition actually had
        for prop in ["offset", "padto", "executable", "provisioning-only"]:
            if prop in partition.props and partition.props[prop].val is not False:
                image_entry[prop.replace("-", "_")] = partition.props[prop].val
        if "source" in partition.props:
            # Special case, skip adding source property if set to AUTO
            if partition.props["source"].val != "$AUTO":
                image_entry["source"] = partition.props["source"].val
        else:
            # Use register address to set source
            image_entry["source"] = str(partition.props["reg"].val[0])

        if label == "failover":
            partitions_yml["fail_over_image"] = image_entry
        else:
            partitions_yml["images"].append(image_entry)

    # Create output file directory
    output_dir = os.path.dirname(args.output_file)
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)

    # Write YAML to output file
    with open(out_yaml, "w", encoding="utf-8") as f:
        yaml_str = yaml.dump(
            partitions_yml, f, default_flow_style=False, sort_keys=False, indent=2
        )

    _logger.debug(f"\nGenerated YAML Content: {args.output_file}\n{yaml_str}")


def invoke_generate_bootfs_yaml(args):
    """
    Generates a boot filesystem YAML file from the partitions node in the
    Zephyr devicetree at build time.

    See parse_args() for a descriptive list of arguments.
    """

    sys.path.insert(
        0,
        str(
            Path(__file__).parents[6]
            / "zephyr"
            / "scripts"
            / "dts"
            / "python-devicetree"
            / "src"
        ),
    )
    from devicetree import edtlib

    if args.verbose:
        logging.basicConfig(format="%(message)s", level=logging.DEBUG)

    edt = edtlib.EDT(args.dts_file, args.bindings_dirs)

    partitions_nodes = edt.compat2nodes.get("tenstorrent,tt-boot-fs")
    partitions_node = partitions_nodes[0]

    if "p300" in args.board:
        _generate_bootfs_yaml(
            args,
            partitions_node,
            args.board.upper() + "-1_left",
            args.board.upper() + "_L",
            args.output_file[:-5] + "_left.yaml",
        )
        _generate_bootfs_yaml(
            args,
            partitions_node,
            args.board.upper() + "-1_right",
            args.board.upper() + "_R",
            args.output_file[:-5] + "_right.yaml",
        )
    else:
        _generate_bootfs_yaml(
            args,
            partitions_node,
            args.board.upper() + "-1",
            args.board.upper(),
            args.output_file,
        )

    return os.EX_OK


def invoke_mkfs(args):
    if not args.specification.exists():
        print(f"Specification file {args.specification} doesn't exist")
        return os.EX_DATAERR
    if args.build_dir and args.build_dir.exists():
        env = {"$ROOT": str(ROOT), "$BUILD_DIR": str(args.build_dir)}
        data = mkfs(args.specification, env, args.hex, args.all)
    else:
        data = mkfs(args.specification, hex=args.hex, all_sections=args.all)
    if data is None:
        return os.EX_DATAERR
    with open(args.output_file, "wb") as file:
        file.write(data)
    print(f"Wrote tt_boot_fs to {args.output_file}")
    return os.EX_OK


def invoke_fsck(args):
    if not args.filesystem.exists():
        print(f"File {args.filesystem} doesn't exist")
        return os.EX_DATAERR
    valid = fsck(args.filesystem)
    print(f"Filesystem {args.filesystem} is {'valid' if valid else 'invalid'}")
    return os.EX_OK


def invoke_ls(args):
    ls(args.bootfs, args.verbose - args.quiet, args.json, args.base64)
    return os.EX_OK


def invoke_extract(args):
    extract(args.bootfs, args.tag, args.output, args.base64)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Utility to manage tt_boot_fs binaries", allow_abbrev=False
    )
    subparsers = parser.add_subparsers()

    generate_bootfs_parser = subparsers.add_parser(
        name="generate_bootfs",
        description="Generate a flash partition YAML from the Zephyr devicetree.",
        allow_abbrev=False,
    )
    generate_bootfs_parser.add_argument(
        "--board",
        required=True,
        help="Type of Tenstorrent Blackhole board.",
    )
    generate_bootfs_parser.add_argument(
        "--dts-file",
        required=True,
        help="Zephyr devicetree file containing the partition node.",
    )
    generate_bootfs_parser.add_argument(
        "--bindings-dirs",
        nargs="+",
        required=True,
        help="Binding directories for the Zephyr devicetree. Should include Zephyr's "
        "bindings and any custom bindings.",
    )
    generate_bootfs_parser.add_argument(
        "--output-file", required=True, help="Output YAML file."
    )
    generate_bootfs_parser.add_argument(
        "--build-dir",
        required=True,
        help="Build directory containing smc, dmc and recovery firmware binaries.",
    )
    generate_bootfs_parser.add_argument(
        "--blobs-dir",
        required=True,
        help="Blobs directory containing ethernet and memory firmware binaries.",
    )
    generate_bootfs_parser.add_argument(
        "--verbose", default=0, action="count", help="Log the YAML file."
    )
    generate_bootfs_parser.set_defaults(func=invoke_generate_bootfs_yaml)

    # MKFS command- build a tt_boot_fs given a specification
    mkfs_parser = subparsers.add_parser("mkfs", help="Make tt_boot_fs filesystem")
    mkfs_parser.add_argument(
        "specification", metavar="SPEC", help="filesystem specification", type=Path
    )
    mkfs_parser.add_argument(
        "output_file", metavar="OUT", help="output file", type=Path
    )
    mkfs_parser.add_argument(
        "--build-dir",
        metavar="BUILD",
        help="build directory to read images from",
        type=Path,
    )
    mkfs_parser.add_argument(
        "--hex", action="store_true", help="Generate intel hex file"
    )
    mkfs_parser.add_argument(
        "--all",
        action="store_true",
        help="Include all bootfs sections, including provisioning only",
    )
    mkfs_parser.set_defaults(func=invoke_mkfs)

    # Check a filesystem for validity
    fsck_parser = subparsers.add_parser("fsck", help="Check tt_boot_fs filesystem")
    fsck_parser.add_argument(
        "filesystem", metavar="FS", help="filesystem to check", type=Path
    )
    fsck_parser.set_defaults(func=invoke_fsck)

    ls_parser = subparsers.add_parser("ls", help="list tt_boot_fs contents")
    ls_parser.add_argument("bootfs", metavar="FS", help="filesystem to list", type=Path)
    ls_parser.add_argument(
        "-b",
        "--base64",
        help="input is base64-encoded",
        default=False,
        action="store_true",
    )
    ls_parser.add_argument(
        "-j", "--json", help="output JSON", default=False, action="store_true"
    )
    ls_parser.add_argument(
        "-q", "--quiet", help="decrease verbosity", default=0, action="count"
    )
    ls_parser.add_argument(
        "-v", "--verbose", help="increase verbosity", default=0, action="count"
    )
    ls_parser.set_defaults(func=invoke_ls)

    extract_parser = subparsers.add_parser(
        "extract", help="extract binary from tt_boot_fs"
    )
    extract_parser.add_argument(
        "bootfs", metavar="FS", help="filesystem to extract from", type=Path
    )
    extract_parser.add_argument(
        "tag", metavar="TAG", help="tt_boot_fs tag to extract binary for", type=str
    )
    extract_parser.add_argument(
        "output", metavar="OUT", help="output path for binary", type=Path
    )
    extract_parser.add_argument(
        "-b",
        "--base64",
        help="input is base64-encoded",
        default=False,
        action="store_true",
    )
    extract_parser.set_defaults(func=invoke_extract)

    # Parse arguments
    args = parser.parse_args()

    if not hasattr(args, "func"):
        print("No command specified")
        parser.print_help()
        sys.exit(os.EX_USAGE)

    return args


def main() -> int:
    args = parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
