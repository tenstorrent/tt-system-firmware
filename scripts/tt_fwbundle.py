#!/usr/bin/env python3

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Tools to manage Tenstorrent firmware bundles.
Supports creating and combining firmware bundles,
as well as extracting their contents.
"""

import argparse
import hashlib
from intelhex import IntelHex
from base64 import b16encode
from pathlib import Path
from typing import Optional
import os
import sys
import shutil
import json
import tarfile
import tempfile

import tt_boot_fs


def bundle_metadata(bundle: Path, board: str = "") -> dict:
    """
    Helper to read firmware bundle file and extract JSON metadata.
    Reads manifest.json and board-specific tt-boot-fs contents.
    """
    data = {}
    try:
        with (
            tarfile.open(bundle, "r:gz") as tar,
            tempfile.TemporaryDirectory() as tempdir,
        ):
            manifest_file = tar.extractfile("./manifest.json")
            data["manifest"] = json.load(manifest_file)
            names = tar.getnames()
            image_files = [name for name in names if "image.bin" in name]
            if board:
                # Filter to specified board only
                image_files = [
                    img for img in image_files if img.startswith(f"./{board}/")
                ]
            if len(image_files) == 0:
                print(f"No images found for board '{board}' in bundle.")
                sys.exit(os.EX_DATAERR)
            for img in image_files:
                board_name = img.split("/")[1]
                board_data = {}
                tar.extract(img, path=tempdir, filter="data")
                try:
                    # Attempt to pull out tt-boot-fs data for this board

                    board_data["bootfs"] = tt_boot_fs.ls(
                        Path(tempdir) / img,
                        output_json=True,
                        input_base64=True,
                        verbose=-1,
                    )
                except Exception:
                    # Ignore errors parsing tt-boot-fs, it won't work for
                    # non-blackhole boards
                    pass
                # Compute sha256 of the bundle binary
                with open(Path(tempdir) / img, "rb") as f:
                    bundle_binary = f.read()
                    board_data["sha256"] = hashlib.sha256(bundle_binary).hexdigest()
                # The compatibility data decides whether this image may be
                # written to a board at all, so it belongs in any comparison
                # of two bundles. Bundles built before board variables have
                # none, and simply do not carry the key.
                try:
                    compat_file = tar.extractfile(
                        f"./{board_name}/compat-variables.json"
                    )
                except KeyError:
                    compat_file = None
                if compat_file is not None:
                    board_data["compat_variables"] = json.load(compat_file)
                data[board_name] = board_data
    except KeyError as e:
        print(f"Firmware bundle missing expected file: {e}")
        sys.exit(os.EX_DATAERR)
    except FileNotFoundError:
        print(f"Firmware bundle file not found: {bundle}")
        sys.exit(os.EX_NOINPUT)
    return data


def extract_bundle_binary(bundle: Path, board: str, tag: str, output: Path):
    """
    Extracts a specific binary from a firmware bundle based on board and image tag.
    """
    try:
        with (
            tarfile.open(bundle, "r:gz") as tar,
            tempfile.TemporaryDirectory() as tempdir,
        ):
            tar.extract(f"./{board}/image.bin", path=tempdir, filter="data")
            return tt_boot_fs.extract(
                Path(tempdir) / f"./{board}/image.bin", tag, output, input_base64=True
            )
    except KeyError as e:
        print(f"Firmware bundle missing expected file: {e}")
        sys.exit(os.EX_DATAERR)
    except FileNotFoundError:
        print(f"Firmware bundle file not found: {bundle}")
        sys.exit(os.EX_NOINPUT)


def ls_fw_bundle(bundle: Path, board: str = "", output_json: bool = False):
    """
    Lists the contents of a firmware bundle file.
    """
    fw_data = bundle_metadata(bundle, board)
    if not output_json:
        print(f"Firmware Bundle: {bundle}")
        manifest = fw_data["manifest"]
        bv = manifest["bundle_version"]
        print(
            f"Bundle Version: {bv['fwId']}.{bv['releaseId']}.{bv['patch']}.{bv['debug']}"
        )
        fw_data.pop("manifest")
        for board_name, fw_data in fw_data.items():
            print("\n" + "=" * 80)
            print(f"Board: {board_name}")
            print(f"Image SHA256: {fw_data['sha256']}")
            if "bootfs" not in fw_data:
                print("No tt-boot-fs data found.")
                continue
            bootfs = fw_data["bootfs"]
            print("tt-boot-fs contents:")
            hdr = "spi_addr\timage_tag\tsize\tcopy_dest\tdata_crc\tflags\t\tfd_crc\t\tdigest"
            print(hdr)
            bar = "-" * len(hdr.expandtabs())
            print(bar)
            for entry in bootfs:
                print(
                    f"{entry['spi_addr']:08x}\t{entry['image_tag']:<8}\t{entry['size']}\t"
                    f"{entry['copy_dest']:08x}\t{entry['data_crc']:08x}\t"
                    f"{entry['flags']:08x}\t{entry['fd_crc']:08x}\t{entry['digest']:.10}"
                )
    else:
        print(json.dumps(fw_data, indent=2))


def build_bootfs_digest_map(bootfs: list[dict]) -> dict[str, str]:
    """
    Builds a map of image tags to digests from a tt-boot-fs listing.
    We prefer digest if present, as these do not change when the image is
    re-signed. If digest is not present, we fall back to using data_crc.
    """
    digest_map = {}
    for entry in bootfs:
        tag = entry["image_tag"]
        digest = entry.get("digest")
        if digest and digest != "N/A":
            digest_map[tag] = digest
        else:
            # Fall back to data_crc if digest not present
            digest_map[tag] = f"crc-{entry['data_crc']:08x}"
    return digest_map


def diff_fw_bundles(bundle1: Path, bundle2: Path):
    """
    Compares two firmware bundles and lists differences.
    """
    fw_data1 = bundle_metadata(bundle1)
    fw_data2 = bundle_metadata(bundle2)

    diff_found = False
    # Compare manifests
    if fw_data1["manifest"] != fw_data2["manifest"]:
        diff_found = True
        print("Manifests differ:")
        print(f"  Bundle 1: {fw_data1['manifest']}")
        print(f"  Bundle 2: {fw_data2['manifest']}")
    # Compare boards
    boards1 = set(fw_data1.keys()) - {"manifest"}
    boards2 = set(fw_data2.keys()) - {"manifest"}
    if boards1 != boards2:
        diff_found = True
        print("Boards differ:")
        print(f"  Bundle 1 boards: {boards1}")
        print(f"  Bundle 2 boards: {boards2}")
    for board in boards1.intersection(boards2):
        board_data1 = fw_data1[board]
        board_data2 = fw_data2[board]
        # Compare tt-boot-fs contents if available
        if "bootfs" in board_data1 and "bootfs" in board_data2:
            map1 = build_bootfs_digest_map(board_data1["bootfs"])
            map2 = build_bootfs_digest_map(board_data2["bootfs"])
            if map1 != map2:
                diff_found = True
                print(f"Board {board} tt-boot-fs contents differ:")
                tags1 = set(map1.keys())
                tags2 = set(map2.keys())
                all_tags = tags1.union(tags2)
                for tag in all_tags:
                    digest1 = map1.get(tag, "<missing>")
                    digest2 = map2.get(tag, "<missing>")
                    if digest1 != digest2:
                        print(f"  Image tag '{tag}' differs:")
                        print(f"    Bundle 1 digest: {digest1}")
                        print(f"    Bundle 2 digest: {digest2}")
        elif board_data1["sha256"] != board_data2["sha256"]:
            diff_found = True
            print(f"Board {board} images differ:")
            print(f"  Bundle 1 SHA256: {board_data1['sha256']}")
            print(f"  Bundle 2 SHA256: {board_data2['sha256']}")
        # Two bundles carrying the same image can still disagree about the
        # hardware it may be written to
        compat1 = board_data1.get("compat_variables", "<missing>")
        compat2 = board_data2.get("compat_variables", "<missing>")
        if compat1 != compat2:
            diff_found = True
            print(f"Board {board} compatibility variables differ:")
            print(f"  Bundle 1: {compat1}")
            print(f"  Bundle 2: {compat2}")
    if not diff_found:
        print("No differences found between the two firmware bundles.")
        return os.EX_OK
    else:
        return os.EX_DATAERR


def combine_fw_bundles(combine: list[Path], output: Path):
    """
    Combines multiple firmware bundle files into a single tar.gz file.
    """
    # Create a temporary directory to extract bundles into
    temp_dir = Path(tempfile.mkdtemp())
    # Extract each bundle into the temp directory
    try:
        for bundle in combine:
            with tarfile.open(bundle, "r:gz") as tar:
                tar.extractall(path=temp_dir, filter="data")
        # Create combined bundle
        if output.exists():
            output.unlink()
        with tarfile.open(output, "x:gz") as tar:
            tar.add(temp_dir, arcname=".")

    except Exception as e:
        raise e
    finally:
        shutil.rmtree(temp_dir)


def create_fw_bundle(
    output: Path,
    version: list[int],
    boot_fs: dict[str, Path] = {},
    compat_variables: Optional[Path] = None,
):
    """
    Creates a firmware bundle tar.gz file, from tt_boot_fs images.
    """
    bundle_dir = Path(tempfile.mkdtemp())
    try:
        # Process (board, tt_boot_fs) pairs
        for board, image in boot_fs.items():
            board_dir = bundle_dir / board
            board_dir.mkdir()
            mask = [{"tag": "write-boardcfg"}]
            with open(board_dir / "mask.json", "w") as file:
                file.write(json.dumps(mask))
            mapping = []
            with open(board_dir / "mapping.json", "w") as file:
                file.write(json.dumps(mapping))
            if compat_variables is not None:
                # Tells the flashing tool which hardware this image is
                # compatible with, beyond the board type that selected it.
                shutil.copyfile(compat_variables, board_dir / "compat-variables.json")
            if image.suffix == ".hex":
                # Encode offsets using @addr format tt-flash supports
                ih = IntelHex(str(image))
                b16out = ""
                for off, end in ih.segments():
                    b16out += f"@{off}\n"
                    b16out += b16encode(
                        ih.tobinarray(start=off, size=end - off)
                    ).decode("ascii")
                    b16out += "\n"
            else:
                with open(image, "rb") as img:
                    binary = img.read()
                    # Convert image to base16 encoded ascii to conform to
                    # tt-flash format
                    b16out = b16encode(binary).decode("ascii")
            with open(board_dir / "image.bin", "w") as img:
                img.write(b16out)

        # Update the manifest last, so we can specify the bundle_version
        manifest = {
            "version": "2.0.0",  # manifest file version
            "bundle_version": {
                "fwId": version[0],
                "releaseId": version[1],
                "patch": version[2],
                "debug": version[3],
            },
        }
        with open(bundle_dir / "manifest.json", "w+") as file:
            file.write(json.dumps(manifest))

        # Compress output as tar.gz
        if output.exists():
            output.unlink()
        with tarfile.open(output, "x:gz") as tar:
            tar.add(bundle_dir, arcname=".")

    except Exception as e:
        raise e
    finally:
        shutil.rmtree(bundle_dir)


def invoke_create_fw_bundle(args):
    # Convert e.g. "80.16.0.1" to [80, 16, 0, 1]
    version = args.version.split(".")
    if len(version) != 4:
        raise RuntimeError("Invalid bundle version format")
    for i in range(4):
        version[i] = int(version[i])
    setattr(args, "version", version)

    # e.g. bootfs["P100-1"] = blah/p100/tt_boot_fs.bin
    if len(args.bootfs) % 2 != 0:
        raise RuntimeError(f"Invalid number of boot fs arguments: {len(args.boot_fs)}")
    bootfs = {}
    for i in range(0, len(args.bootfs), 2):
        bootfs[args.bootfs[i]] = Path(args.bootfs[i + 1])
    setattr(args, "bootfs", bootfs)

    create_fw_bundle(args.output, args.version, args.bootfs, args.compat_variables)
    print(f"Wrote fwbundle to {args.output}")
    return os.EX_OK


def invoke_combine_fw_bundle(args):
    combine_fw_bundles(args.bundles, args.output)
    print(f"Wrote combined fwbundle to {args.output}")
    return os.EX_OK


def invoke_ls_fw_bundle(args):
    ls_fw_bundle(args.bundle, board=args.board, output_json=args.json)
    return os.EX_OK


def invoke_diff_fw_bundles(args):
    return diff_fw_bundles(args.bundle1, args.bundle2)


def invoke_extract_fw_bundle(args):
    ret = extract_bundle_binary(args.bundle, args.board, args.tag, args.output)
    if ret == os.EX_OK:
        print(f"Extracted image '{args.tag}' for board '{args.board}' to {args.output}")
    return ret


def parse_args():
    """
    Parse command line arguments.
    """
    parser = argparse.ArgumentParser(
        description="Tools to manage Tenstorrent firmware bundles.",
        allow_abbrev=False,
    )
    subparsers = parser.add_subparsers()

    # Create a firmware bundle
    fw_bundle_create_parser = subparsers.add_parser(
        "create", help="Create a firmware bundle"
    )
    fw_bundle_create_parser.set_defaults(func=invoke_create_fw_bundle)
    fw_bundle_create_parser.add_argument(
        "-v",
        "--version",
        metavar="VERSION",
        help="bundle version (e.g. 80.16.0.1)",
        required=True,
    )
    fw_bundle_create_parser.add_argument(
        "-o",
        "--output",
        metavar="BUNDLE",
        help="output bundle file name",
        type=Path,
        required=True,
    )
    fw_bundle_create_parser.add_argument(
        "--compat-variables",
        metavar="JSON",
        help="compat-variables.json describing the hardware the image supports",
        type=Path,
        default=None,
    )
    fw_bundle_create_parser.add_argument(
        "bootfs",
        metavar="BOARD_FS",
        help="[PREFIX FS..] pairs (e.g. P150A-1 build-p150a/tt_boot_fs.bin)",
        nargs="*",
        default=[],
    )
    # Combine multiple firmware bundles
    fw_bundle_combine_parser = subparsers.add_parser(
        "combine", help="Combine multiple firmware bundles"
    )
    fw_bundle_combine_parser.set_defaults(func=invoke_combine_fw_bundle)
    fw_bundle_combine_parser.add_argument(
        "-o",
        "--output",
        metavar="BUNDLE",
        help="output bundle file name",
        type=Path,
        required=True,
    )
    fw_bundle_combine_parser.add_argument(
        "bundles",
        metavar="BUNDLES",
        help="input bundle files to combine",
        nargs="+",
        default=[],
    )
    # List contents of a firmware bundle
    fw_bundle_ls_parser = subparsers.add_parser(
        "ls", help="List contents of a firmware bundle"
    )
    fw_bundle_ls_parser.set_defaults(func=invoke_ls_fw_bundle)
    fw_bundle_ls_parser.add_argument(
        "-b",
        "--board",
        default="",
        metavar="BOARD",
        help="board prefix to list (e.g. P150A-1)",
    )
    fw_bundle_ls_parser.add_argument(
        "-j",
        "--json",
        help="output in JSON format",
        action="store_true",
    )
    fw_bundle_ls_parser.add_argument(
        "bundle",
        metavar="BUNDLE",
        help="input bundle file to list",
        type=Path,
    )
    # Diff two firmware bundles
    fw_bundle_diff_parser = subparsers.add_parser(
        "diff", help="Diff two firmware bundles"
    )
    fw_bundle_diff_parser.set_defaults(func=invoke_diff_fw_bundles)
    fw_bundle_diff_parser.add_argument(
        "bundle1",
        metavar="BUNDLE1",
        help="first bundle file to compare",
        type=Path,
    )
    fw_bundle_diff_parser.add_argument(
        "bundle2",
        metavar="BUNDLE2",
        help="second bundle file to compare",
        type=Path,
    )
    # Extract a binary from a firmware bundle
    fw_bundle_extract_parser = subparsers.add_parser(
        "extract", help="Extract a binary from a firmware bundle"
    )
    fw_bundle_extract_parser.set_defaults(func=invoke_extract_fw_bundle)
    fw_bundle_extract_parser.add_argument(
        "bundle",
        metavar="BUNDLE",
        help="input bundle file to extract from",
        type=Path,
    )
    fw_bundle_extract_parser.add_argument(
        "-b",
        "--board",
        metavar="BOARD",
        help="board prefix (e.g. P150A-1)",
        required=True,
    )
    fw_bundle_extract_parser.add_argument(
        "-t",
        "--tag",
        metavar="TAG",
        help="image tag to extract (e.g. bootloader)",
        required=True,
    )
    fw_bundle_extract_parser.add_argument(
        "-o",
        "--output",
        metavar="OUTPUT",
        help="output file for extracted binary",
        type=Path,
        required=True,
    )

    args = parser.parse_args()
    if not hasattr(args, "func"):
        print("No command specified")
        parser.print_help()
        sys.exit(os.EX_USAGE)

    return args


def main():
    """
    Main entry point for tt_fwbundle script.
    """
    args = parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
