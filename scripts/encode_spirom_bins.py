# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

# run instructions for this script, need the export because os.envirm doesn't have global scope:
# export PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python && python encode_spirom_bins.py -f <folder_name>

import os
import sys
import argparse
import zlib
from typing import Optional
from google.protobuf import text_format
from tt_boot_fs import cksum

BOARD_TXT_CONFIG_PATH = os.path.join(
    os.path.dirname(sys.argv[0]),
    "../boards/tenstorrent/tt_blackhole/spirom_data_tables",
)

# check for os.env PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION and error out if not set
if os.environ.get("PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION") != "python":
    print("Please run the following command before running this script:")
    print("export PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python")
    sys.exit(1)


def check_encode_pad_message(message, prepend_checksum: bool):
    encoded_message = message.SerializeToString()

    # Need to pad the value to be divisible by 4
    num_bytes_to_pad = 4 - len(encoded_message) % 4

    # Pad the message with 0x0 - 0x(num_bytes_to_pad - 1) numbers
    # Reference - https://tenstorrent.atlassian.net/wiki/spaces/syseng/pages/448102409/Protobufs+SPIROM
    encoded_message += bytes(range(num_bytes_to_pad))
    # print(f"Encoded message for {message_name}: {encoded_message}")
    # text_format.PrintMessage(message, sys.stdout)

    if prepend_checksum:
        encoded_message = (cksum(encoded_message) & 0xFFFF_FFFF).to_bytes(
            4, "little"
        ) + encoded_message

    return encoded_message


def write_bin_to_file(in_folder, out_folder, bin_file_name, encoded_data):
    # Write the serialized data to a binary file
    os.makedirs(out_folder, exist_ok=True)
    os.makedirs(f"{out_folder}/{in_folder}", exist_ok=True)
    with open(f"{out_folder}/{in_folder}/{bin_file_name}", "wb") as f:
        f.write(encoded_data)
    return f"{out_folder}/{in_folder}/{bin_file_name}"


CCFGOVR_MAGIC = 0x564F4343
CCFGOVR_HDR_VERSION = 0


def build_empty_ccfgovr_bin() -> bytes:
    seq = 0
    body_len = 0
    body = b""
    header_no_cksum = (
        CCFGOVR_MAGIC.to_bytes(4, "little")
        + seq.to_bytes(4, "little")
        + body_len.to_bytes(4, "little")
        + CCFGOVR_HDR_VERSION.to_bytes(4, "little")
    )
    crc = zlib.crc32(header_no_cksum + body)
    return header_no_cksum + crc.to_bytes(4, "little") + body


# Override is a set of key, value pairs used to override normal text fields...
# Currently it just supports single level of setting
def convert_proto_txt_to_bin_file(
    board_cfg_path,
    in_folder,
    out_folder,
    message_name,
    message_type,
    prepend_checksum: bool,
    override: Optional[dict] = None,
):
    proto_txt_table = f"{board_cfg_path}/{in_folder}/{message_name}.txt"
    with open(proto_txt_table, "r") as file:
        template = file.read()

    print(f"Checking proto_txt_table for {message_name}...")
    message_from_template = message_type()
    message = text_format.Parse(template, message_from_template)

    if override is not None:
        for key, v in override.items():
            setattr(message, key, v)

    # Parse message would raise exception if the template is invalid
    print(f"{message_name} proto_txt_table is valid")

    encoded_message = check_encode_pad_message(message, prepend_checksum)

    filename = write_bin_to_file(
        in_folder, out_folder, f"{message_name}.bin", encoded_message
    )
    print(f"{message_name} table has been written to a binary file {filename}")
    print()


def decode_bundle_version(bundle_version: str) -> int:
    try:
        major, minor, patch, rc = map(int, bundle_version.split("."))
        return (major << 24) | (minor << 16) | (patch << 8) | rc
    except ValueError:
        raise ValueError(
            "Invalid bundle version format. Expected format: major.minor.patch.rc"
        )


def main():
    parser = argparse.ArgumentParser(
        description="Encode SPIROM bins", allow_abbrev=False
    )
    parser.add_argument(
        "-b",
        "--board",
        type=str,
        help="Name of the board you are building binary files for",
        required=True,
    )
    parser.add_argument(
        "-o",
        "--output",
        type=str,
        default="generated_proto_bins",
        help="Folder name to dump the generated binaries",
    )
    parser.add_argument(
        "-s",
        "--build-dir",
        type=str,
        default="build",
        help="Name of build folder with generated protobuf files",
    )
    parser.add_argument(
        "--bundle-version",
        type=str,
        default="0.0.0.0",
        required=True,
        help="Firmware bundle version to encode",
    )
    args = parser.parse_args()
    build_folder = os.path.join(args.build_dir, "zephyr/python_proto_files")
    if not os.path.exists(build_folder):
        print(
            f"Build folder does not exist, please build smc with west first: {build_folder}"
        )
        sys.exit(1)
    sys.path.append(build_folder)
    try:
        import fw_table_pb2
        import read_only_pb2
        import flash_info_pb2
    except ImportError as e:
        print(f"Error importing protobuf modules: {e}")
        print("Ensure the protobuf files are generated and the path is correct.")
        sys.exit(1)
    bundle_version_int = decode_bundle_version(args.bundle_version)
    convert_proto_txt_to_bin_file(
        BOARD_TXT_CONFIG_PATH,
        args.board,
        args.output,
        "fw_table",
        fw_table_pb2.FwTable,
        False,
        override={"fw_bundle_version": bundle_version_int},
    )
    convert_proto_txt_to_bin_file(
        BOARD_TXT_CONFIG_PATH,
        args.board,
        args.output,
        "flash_info",
        flash_info_pb2.FlashInfoTable,
        False,
    )
    convert_proto_txt_to_bin_file(
        BOARD_TXT_CONFIG_PATH,
        args.board,
        args.output,
        "read_only",
        read_only_pb2.ReadOnly,
        False,
    )

    ccfgovr_path = write_bin_to_file(
        args.board, args.output, "ccfgovr.bin", build_empty_ccfgovr_bin()
    )
    print(f"CCFGOVR initial empty bank written to {ccfgovr_path}")


if __name__ == "__main__":
    main()
