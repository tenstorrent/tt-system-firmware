# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Tooling for editing firmware tables in blackhole firmware bundles.
"""

import os
import logging
import struct
import subprocess
import sys
import tempfile
import tarfile
import shutil

from base64 import b16decode, b16encode
from intelhex import IntelHex
from pathlib import Path

BOOTFS_FWTABLE_NAME = "cmfwcfg"
TT_Z_P_ROOT = Path(__file__).parents[1]

# Multi-table boot filesystem header. Images without it have a single
# descriptor table at tt_boot_fs.FD_HEAD_ADDR.
BOOTFS_HEADER_ADDR = 0x120000
BOOTFS_HEADER_MAGIC = 0x54544246
BOOTFS_HEADER_VERSION = 1
BOOTFS_TABLE_COUNT_MAX = 16

# Images start on an erase block boundary, so an image may grow up to the end
# of its last block without reaching the next image.
BOOTFS_BLOCK_SIZE = 0x1000

sys.path.append(str(TT_Z_P_ROOT / "scripts"))

import tt_boot_fs  # noqa: E402

_logger = logging.getLogger(__name__)


# NOTE: A small framing layer was needed to bridge Google protobuf and nanopb functionality
# due to the PB_DECODE_NULLTERMINATED framing method.
#
# Nanopb PB_ENCODE_NULLTERMINATED adds a single 0 byte to the end of an endoded message.
# PB_DECODE_NULLTERMINATED reads that as a 0 tag and ends. google protobuf doesn’t support this.
# To allow google protobuf use we: add the 0 and then pad to a multiple of 4 with 0 followed by
# the number of padding bytes added. Before google PB decode, look at the last byte.
#
# 0: remove 1 byte (0)
# 1: remove 2 bytes (0, 1)
# 2: remove 3 bytes (0, X, 2)
# 3: remove 4 bytes (0, X, X, 3)
def nanopb_remove_framing(b: bytes) -> bytes:
    last_byte = b[-1]
    bytes_to_remove = last_byte + 1
    return b[:-bytes_to_remove]


def nanopb_add_framing(b: bytes) -> bytes:
    # duplicate b, so that we leave the original 'b' object unmodified
    c = b[:]
    bytes_to_add = 4 - (len(c) % 4)
    c += bytes(range(bytes_to_add))
    return c


class BootFsImage:
    """
    A base16 encoded tt-boot-fs image from a firmware bundle, edited in place.

    Descriptor tables are located from the on-flash layout rather than through
    tt_boot_fs.BootFs, so that the published copy of this tooling can edit
    images with either the single-table or the multi-table layout. Only the
    edited image and its descriptor change; every other byte is written back
    as it was read.
    """

    def __init__(self, path: Path):
        self.ih = IntelHex()
        offset = 0
        with open(path, "r") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                if line.startswith("@"):
                    offset = int(line[1:], 10)
                else:
                    data = b16decode(line)
                    self.ih.puts(offset, data)
                    offset += len(data)

    def save(self, path: Path):
        with open(path, "w") as f:
            for start, end in self.ih.segments():
                f.write(f"@{start}\n")
                data = self.ih.tobinstr(start=start, size=end - start)
                f.write(b16encode(data).decode("ascii"))
                f.write("\n")

    def _read(self, addr: int, size: int) -> bytes:
        return self.ih.tobinstr(start=addr, size=size)

    def table_addrs(self) -> list[int]:
        header_fmt = "<III"
        magic, version, num_tables = struct.unpack(
            header_fmt, self._read(BOOTFS_HEADER_ADDR, struct.calcsize(header_fmt))
        )
        if magic != BOOTFS_HEADER_MAGIC:
            return [tt_boot_fs.FD_HEAD_ADDR]

        if version != BOOTFS_HEADER_VERSION:
            raise ValueError(
                f"unsupported bootfs version {version}; expected {BOOTFS_HEADER_VERSION}"
            )
        if not 0 < num_tables <= BOOTFS_TABLE_COUNT_MAX:
            raise ValueError(f"invalid bootfs table count {num_tables}")

        addrs_fmt = f"<{num_tables}I"
        return list(
            struct.unpack(
                addrs_fmt,
                self._read(
                    BOOTFS_HEADER_ADDR + struct.calcsize(header_fmt),
                    struct.calcsize(addrs_fmt),
                ),
            )
        )

    def find(self, tag: str) -> tuple[int, "tt_boot_fs.tt_boot_fs_fd"]:
        """
        Return the flash address and contents of the descriptor for @p tag,
        searching every descriptor table.
        """
        matches = []
        for table_addr in self.table_addrs():
            for fd_addr, fd in tt_boot_fs.iter_fd(
                lambda addr, size, table_addr=table_addr: self._read(
                    table_addr + addr, size
                )
            ):
                if fd.image_tag_str() == tag:
                    matches.append((table_addr + fd_addr, fd))

        if not matches:
            raise KeyError(f"no '{tag}' entry found in any bootfs table")
        if len(matches) > 1:
            raise ValueError(f"found {len(matches)} '{tag}' entries, expected one")

        fd_addr, fd = matches[0]
        if fd.fd_crc != tt_boot_fs.cksum(bytes(fd)[: -tt_boot_fs.CKSUM_SIZE]):
            raise ValueError(f"'{tag}' descriptor at 0x{fd_addr:x} is corrupt")

        return fd_addr, fd

    def read(self, tag: str) -> tuple[int, bytes]:
        """
        Return the flash address and contents of the image for @p tag.
        """
        _, fd = self.find(tag)
        data = self._read(fd.spi_addr, fd.flags.f.image_size)
        if fd.data_crc != tt_boot_fs.cksum(data):
            raise ValueError(f"'{tag}' image at 0x{fd.spi_addr:x} is corrupt")

        return fd.spi_addr, data

    def write(self, tag: str, data: bytes):
        """
        Replace the image for @p tag with @p data, at the same flash address.
        """
        fd_addr, fd = self.find(tag)
        old_end = fd.spi_addr + fd.flags.f.image_size
        limit = (old_end + BOOTFS_BLOCK_SIZE - 1) & ~(BOOTFS_BLOCK_SIZE - 1)
        if fd.spi_addr + len(data) > limit:
            raise ValueError(
                f"{len(data)} bytes for '{tag}' at 0x{fd.spi_addr:x} would overrun 0x{limit:x}"
            )

        del self.ih[fd.spi_addr : old_end]
        self.ih.puts(fd.spi_addr, data)

        fd.flags.f.image_size = len(data)
        fd.data_crc = tt_boot_fs.cksum(data)
        fd.fd_crc = tt_boot_fs.cksum(bytes(fd)[: -tt_boot_fs.CKSUM_SIZE])
        self.ih.puts(fd_addr, bytes(fd))


def get_board_names_with_cmfwcfg(bundle_dir: Path) -> set[str]:
    board_names = set()
    for image_path in bundle_dir.glob("*/image.bin"):
        try:
            BootFsImage(image_path).find(BOOTFS_FWTABLE_NAME)
        except (KeyError, ValueError):
            # Not a tt-boot-fs image, e.g. a Wormhole board
            continue
        board_names.add(image_path.parent.name)

    return board_names


def print_board_names(board_names: set[str]):
    # List available board names and exit
    if not board_names:
        _logger.warning(f"No boards found with bootfs entry {BOOTFS_FWTABLE_NAME}")

    for board in board_names:
        _logger.info(board)


def filter_boards(arg_boards: list[str], board_names: set[str]) -> set[str]:
    # Filter supplied board names to only those that apply
    for b in arg_boards:
        if b not in board_names:
            _logger.warning(
                f"board '{b}' does not have a bootfs entry {BOOTFS_FWTABLE_NAME} and will be skipped"
            )
            continue

    return {b for b in arg_boards if b in board_names}


def do_update(
    input: Path,
    output: Path,
    board: list[str],
    update_cb,
    cb_object,
    verbose: bool = False,
) -> int:
    if verbose:
        logging.basicConfig(format="%(message)s", level=logging.DEBUG)

    # Generate necessary protobuf python bindings
    with tempfile.TemporaryDirectory() as proto_tempdir:
        _logger.debug("Generating protobuf python bindings for fw_table_pb2..")

        os.environ["PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION"] = "python"

        OUTPUT_DIR = Path(proto_tempdir)
        OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
        SPIROM_PROTOBUFS = Path(__file__).parent
        if not (SPIROM_PROTOBUFS / "fw_table.proto").exists():
            # Fallback to fixed path
            SPIROM_PROTOBUFS = (
                TT_Z_P_ROOT / "drivers" / "misc" / "bh_fwtable" / "spirom_protobufs"
            )

        protoc_path = (
            TT_Z_P_ROOT / ".." / "modules" / "lib" / "nanopb" / "generator" / "protoc"
        )
        if not protoc_path.exists():
            protoc_path = Path(shutil.which("protoc"))

        if not protoc_path.exists():
            _logger.error(
                "protoc compiler not found. Please ensure it is installed and on your PATH."
            )
            return os.EX_UNAVAILABLE

        try:
            subprocess.run(
                [
                    protoc_path,
                    # Required by protoc < 3.15, e.g. Ubuntu 22.04's
                    "--experimental_allow_proto3_optional",
                    f"--python_out={OUTPUT_DIR}",
                    f"{SPIROM_PROTOBUFS}/fw_table.proto",
                    "-I",
                    f"{SPIROM_PROTOBUFS}",
                ],
                capture_output=True,
                check=True,
            )
        except subprocess.CalledProcessError as e:
            _logger.error(
                f"Error generating protobuf python bindings: {e.stderr.decode()}"
            )
            return os.EX_UNAVAILABLE
        sys.path.append(str(OUTPUT_DIR))
        import fw_table_pb2  # noqa: E402

        with tempfile.TemporaryDirectory() as tempdir:
            try:
                _logger.info(f"Loading fwbundle {input}")
                with tarfile.open(input, "r:gz") as tar_in:
                    tar_in.extractall(path=tempdir)
            except Exception as e:
                _logger.error(f"Error loading fwbundle: {e}")
                return os.EX_DATAERR

            board_names = get_board_names_with_cmfwcfg(Path(tempdir))
            if not board:
                # No board names have been supplied, so use all of them that apply
                board = board_names

            if "?" in board:
                print_board_names(board_names)
                return os.EX_OK

            # Ensure only valid board names are used
            board = filter_boards(board, board_names)

            if not board:
                _logger.info("No valid boards to process")
                return os.EX_DATAERR

            # Iterate over each board to extract the cmfwcfg entry, modify it, and write it back
            for b in board:
                _logger.debug(f"Processing board '{b}'")
                ret = update_cb(
                    cb_object,
                    fw_table_pb2,
                    Path(tempdir) / b / "image.bin",
                    verbose=verbose,
                )
                if ret != os.EX_OK:
                    return ret

            # Create a new fwbundle with the modified files
            verb = "Creating"
            if output.exists():
                verb = "Overwriting existing"
                os.remove(output)

            _logger.info(f"{verb} output fwbundle: {output}")
            with tarfile.open(output, "x:gz") as tar_out:
                tar_out.add(tempdir, arcname=".")

        # verify that the firmware table includes the expected changes
        with tarfile.open(output, "r:gz") as tar_in:
            with tempfile.TemporaryDirectory() as tempdir:
                tar_in.extractall(path=tempdir)

                # Iterate over each board to extract the cmfwcfg entry, modify it, and write it back
                for b in board:
                    _logger.debug(f"Verifying board '{b}'")
                    ret = update_cb(
                        cb_object,
                        fw_table_pb2,
                        Path(tempdir) / b / "image.bin",
                        verbose=verbose,
                        verify=True,
                    )
                    if ret != os.EX_OK:
                        return ret

    return os.EX_OK
