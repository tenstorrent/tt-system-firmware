#!/usr/bin/env python3
# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
This file generates an mcuboot trailer for Zephyr images.
It can generate a trailer for a confirmed image, or an
image to boot in test mode.
Only images with BOOT_MAX_ALIGN==8 are supported.
"""

import argparse

# Taken from mcuboot design documents
BOOT_MAGIC_ALIGN_8 = [
    0x77,
    0xC2,
    0x95,
    0xF3,
    0x60,
    0xD2,
    0xEF,
    0x7F,
    0x35,
    0x52,
    0x50,
    0x0F,
    0x2C,
    0xB6,
    0x79,
    0x80,
]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Generate an mcuboot trailer for Zephyr images",
        allow_abbrev=False,
    )
    parser.add_argument(
        "--confirmed",
        action="store_true",
        help="Generate a trailer for a confirmed image",
    )
    parser.add_argument(
        "output",
        help="Output file to write the trailer to",
    )
    return parser.parse_args()


def main():
    args = parse_args()

    # Pad the trailer with 4056 bytes of 0xff
    trailer = bytearray([0xFF] * 4056)

    # Add the swap-info, image-ok, and copy-done bytes (8 bytes each)
    trailer += bytearray([0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF])  # swap-info
    # A confirmed trailer also ships copy-done set. MCUBoot writes that byte
    # itself the first time it boots the slot, which would leave the trailer on
    # flash one byte away from the one in the firmware bundle and no longer
    # matching its own boot filesystem CRC. tt-flash rewrites a boot-critical
    # image in that state, so the recovery trailer would be erased and rewritten
    # by every subsequent update -- on exactly the boards that have already had
    # to fall back to recovery. Shipping the byte set makes MCUBoot's write a
    # no-op and keeps the trailer identical to the bundle for good.
    trailer += bytearray(
        [0x01 if args.confirmed else 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]
    )  # copy-done
    trailer += bytearray(
        [0x01 if args.confirmed else 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]
    )  # image-ok

    # Add the magic number
    trailer += bytearray(BOOT_MAGIC_ALIGN_8)

    # Write the trailer to the output file
    with open(args.output, "wb") as f:
        f.write(trailer)


if __name__ == "__main__":
    main()
