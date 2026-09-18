# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Tooling for editing firmware tables in blackhole firmware bundles.
"""

import os
import logging
import subprocess
import sys
import tempfile
import tarfile
import shutil

from pathlib import Path

BOOTFS_FWTABLE_NAME = "cmfwcfg"
TT_Z_P_ROOT = Path(__file__).parents[1]

sys.path.append(str(TT_Z_P_ROOT / "scripts"))

import tt_fwbundle  # noqa: E402

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


def get_board_names_with_cmfwcfg_from_bundle_metadata(bundle_metadata) -> set[str]:
    board_names = []
    for meta_name, meta_data in bundle_metadata.items():
        if meta_name == "manifest":
            # not a board
            continue

        board_name = meta_name
        for table in meta_data["bootfs"].values():
            for part in table:
                if part["image_tag"] == BOOTFS_FWTABLE_NAME:
                    board_names.append(board_name)
                    break

    return set(board_names)


def get_board_metadata_from_bundle_metadata(
    bundle_metadata, board_names
) -> dict[str, dict]:
    board_meta = {}

    for meta_name, meta_data in bundle_metadata.items():
        if meta_name == "manifest":
            # not a board
            continue

        board_name = meta_name
        if board_name not in board_names:
            continue

        board_meta[board_name] = meta_data

    return board_meta


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


def get_cmfwcfg_from_bootfs_metadata(bootfs):
    for part in bootfs:
        if part["image_tag"] == BOOTFS_FWTABLE_NAME:
            return part

    return None


def get_cmfwcfg_table(bootfs):
    table_idx = None
    for idx, table in enumerate(bootfs.tables):
        if BOOTFS_FWTABLE_NAME in table.entries:
            table_idx = idx
            break

    if table_idx is None:
        _logger.error(f"{BOOTFS_FWTABLE_NAME} not found in bootfs")
        return None

    return table_idx


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

        try:
            _logger.info(f"Loading fwbundle {input}")
            bundle_metadata = tt_fwbundle.bundle_metadata(input)
        except Exception as e:
            _logger.error(f"Error loading fwbundle metadata: {e}")
            return os.EX_DATAERR

        board_names = get_board_names_with_cmfwcfg_from_bundle_metadata(bundle_metadata)
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

        with tarfile.open(input, "r:gz") as tar_in:
            with tempfile.TemporaryDirectory() as tempdir:
                tar_in.extractall(path=tempdir)

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
