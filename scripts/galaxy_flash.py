# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import argparse
from intelhex import IntelHex
from pathlib import Path
import subprocess
import re
from tqdm import tqdm

# Command code constants
NETFN = 0x32  # OEM NetFn 2 (used for flash operations)
CMD_READ_FLASH = 0x77
CMD_WRITE_FLASH = 0x78
CMD_ERASE_FLASH = 0x79

SPI_BLOCK_SIZE = 4096


def send_raw_ipmi_command(
    netfn, cmd, data=None, host=None, username=None, password=None, timeout=30
):
    """
    Send raw IPMI command using ipmitool subprocess and return bytes

    @param netfn (int): Network Function
    @param cmd (int): Command
    @param data (list/int/bytes, optional): Command data payload
    @param host (str, optional): Remote BMC IP
    @param username (str, optional): Username for remote connection
    @param password (str, optional): Password for remote connection
    @param timeout (int): Command timeout in seconds

    @return dict: {
            'success': bool,
            'completion_code': int,
            'data': bytes,
            'error': str (if any)
        }
    """
    try:
        # Build command
        cmd_list = ["ipmitool"]

        # Add remote connection parameters
        if host and username and password:
            cmd_list.extend(
                ["-I", "lanplus", "-H", host, "-U", username, "-P", password]
            )

        # Add raw command
        cmd_list.extend(["raw", f"0x{netfn:02x}", f"0x{cmd:02x}"])

        # Add data payload
        if data is not None:
            if isinstance(data, (list, tuple)):
                cmd_list.extend([f"0x{x:02x}" for x in data])
            elif isinstance(data, int):
                cmd_list.append(f"0x{data:02x}")
            elif isinstance(data, (bytes, bytearray)):
                cmd_list.extend([f"0x{x:02x}" for x in data])

        # Execute command
        result = subprocess.run(
            cmd_list, capture_output=True, text=True, timeout=timeout
        )

        if result.returncode == 0:
            # Parse hex output to bytes
            hex_output = result.stdout.strip()
            if hex_output:
                # Remove any whitespace and split hex values
                hex_values = re.findall(r"[0-9a-fA-F]{2}", hex_output)
                data_bytes = bytes([int(x, 16) for x in hex_values])
            else:
                data_bytes = b""

            return {
                "success": True,
                "completion_code": 0,  # Successful ipmitool execution
                "data": data_bytes,
                "error": None,
            }
        else:
            return {
                "success": False,
                "completion_code": result.returncode,
                "data": b"",
                "error": result.stderr.strip(),
            }

    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "completion_code": None,
            "data": b"",
            "error": "Command timed out",
        }
    except Exception as e:
        return {"success": False, "completion_code": None, "data": b"", "error": str(e)}


def read_flash(ubb_idx, asic_idx, address, length, progress_print=False):
    """
    Read data from flash memory at specified address
    @ param ubb_idx (int): UBB index
    @ param asic_idx (int): ASIC index
    @ param address (int): Flash memory address to read from
    @ param length (int): Number of bytes to read
    @ return bytes read from flash memory, or raise exception on failure
    """
    read_data = bytearray()
    read_addr = address
    read_len = length

    if read_addr % 256 != 0:
        raise ValueError("Read address must be aligned to 256 bytes")

    with tqdm(
        total=length,
        desc="Reading",
        unit="B",
        unit_scale=True,
        disable=not progress_print,
    ) as pbar:
        while read_len > 0:
            result = send_raw_ipmi_command(
                NETFN,
                CMD_READ_FLASH,
                data=[
                    ubb_idx,  # arg0: UBB index
                    asic_idx,  # arg1: ASIC index
                    (read_addr & 0xFF),  # arg2: Address low byte
                    ((read_addr >> 8) & 0xFF),  # arg3: Address mid byte
                    ((read_addr >> 16) & 0xFF),  # arg4: Address high byte
                    0x0,  # arg5: bytes to read (1-256, 0 means 256 bytes)
                ],
            )
            if not result["success"]:
                raise RuntimeError(
                    f"Failed to read flash block at 0x{read_addr:x}: {result['error']}"
                )

            read_data.extend(result["data"])
            pbar.update(min(256, read_len))
            read_addr += 256
            read_len -= 256

    return bytes(read_data[:length])


def program_flash_block(ubb_idx, asic_idx, write_addr, data):
    """
    Program flash block (4K length) to given address
    @ param ubb_idx (int): UBB index
    @ param asic_idx (int): ASIC index
    @ param write_addr (int): Address to write to
    @ param data (bytes): Bytes to program to address
    """

    write_offset = 0
    if len(data) != SPI_BLOCK_SIZE:
        raise ValueError("Flash block can only be 4K bytes")

    # Erase 4K block, then program in 128 byte chunks
    ipmi_data = [
        ubb_idx,  # arg0: UBB index
        asic_idx,  # arg1: ASIC index
        (write_addr & 0xFF),  # arg2: Address low byte
        ((write_addr >> 8) & 0xFF),  # arg3: Address mid byte
        ((write_addr >> 16) & 0xFF),  # arg4: Address high byte
    ]
    result = send_raw_ipmi_command(NETFN, CMD_ERASE_FLASH, data=ipmi_data)
    if not result["success"]:
        raise RuntimeError(
            f"Failed to erase flash block at 0x{write_addr:x}: {result['error']}"
        )

    # Write 32 chunks of 128 bytes (total 4K block)
    for _ in range(32):
        ipmi_data = (
            [
                ubb_idx,  # arg0: UBB index
                asic_idx,  # arg1: ASIC index
                (write_addr & 0xFF),  # arg2: Address low byte
                ((write_addr >> 8) & 0xFF),  # arg3: Address mid byte
                ((write_addr >> 16) & 0xFF),  # arg4: Address high byte
            ]
            + list(data[write_offset : write_offset + 128])
        )  # arg5+: Data payload

        result = send_raw_ipmi_command(NETFN, CMD_WRITE_FLASH, data=ipmi_data)
        if not result["success"]:
            raise RuntimeError(
                f"Failed to write flash block at 0x{write_addr:x}: {result['error']}"
            )
        write_addr += 128
        write_offset += 128


def write_flash(ubb_idx, asic_idx, ih):
    """
    Write data to flash memory at specified address
    @ param ubb_idx (int): UBB index
    @ param asic_idx (int): ASIC index
    @ param ih: Intel Hex object with data to program
    @ return None, or raise exception on failure
    """
    total_len = sum(segment[1] - segment[0] for segment in ih.segments())

    with tqdm(total=total_len, desc="Writing", unit="B", unit_scale=True) as pbar:
        for segment in ih.segments():
            write_len = segment[1] - segment[0]
            write_addr = segment[0]

            # Program segment in 3 phases:
            # 1. Unaligned section before first 4K boundary (if needed)
            # 2. Aligned 4K blocks
            # 3. Unaligned section after last 4K boundary (if needed)

            # Phase 1: Handle unaligned start address by programming up to next 4K block boundary
            if write_addr % SPI_BLOCK_SIZE != 0:
                # Program up to 4K block boundary
                block_boundary = (write_addr + 4095) // SPI_BLOCK_SIZE * SPI_BLOCK_SIZE
                phase1_len = min(write_len, block_boundary - write_addr)
                flash_data = read_flash(
                    ubb_idx, asic_idx, block_boundary - SPI_BLOCK_SIZE, SPI_BLOCK_SIZE
                )
                flash_data = bytes(
                    flash_data[0 : write_addr - block_boundary + SPI_BLOCK_SIZE]
                ) + ih.tobinarray(start=write_addr, size=phase1_len)
                program_flash_block(
                    ubb_idx, asic_idx, block_boundary - SPI_BLOCK_SIZE, flash_data
                )
                write_addr = block_boundary
                write_len -= phase1_len
                pbar.update(phase1_len)

            # Phase 2: Aligned 4K blocks
            while (
                write_addr < ((segment[1] // SPI_BLOCK_SIZE) * SPI_BLOCK_SIZE)
                and write_len >= SPI_BLOCK_SIZE
            ):
                # Program 4K blocks
                flash_data = ih.tobinarray(write_addr, size=SPI_BLOCK_SIZE)
                program_flash_block(ubb_idx, asic_idx, write_addr, flash_data)
                write_addr += SPI_BLOCK_SIZE
                write_len -= SPI_BLOCK_SIZE
                pbar.update(SPI_BLOCK_SIZE)

            # Phase 3: Handle unaligned programming after 4K boundary
            if write_len > 0:
                flash_data = read_flash(ubb_idx, asic_idx, write_addr, SPI_BLOCK_SIZE)
                flash_data = (
                    bytes(ih.tobinarray(start=write_addr, size=write_len))
                    + flash_data[write_len:SPI_BLOCK_SIZE]
                )
                program_flash_block(ubb_idx, asic_idx, write_addr, flash_data)
                pbar.update(write_len)


def check_bmc_version():
    """
    Check BMC version to ensure compatibility with flash operations
    @ return dict: {
            'success': bool,
            'version': str (if success),
            'error': str (if failure)
        }
    """
    try:
        result = subprocess.run(
            ["ipmitool", "mc", "info"], capture_output=True, text=True, timeout=10
        )
        if result.returncode != 0:
            return {"success": False, "version": None, "error": result.stderr.strip()}

        match = re.search(r"Firmware Revision\s*:\s*(\d+)\.(\d+)", result.stdout)
        if not match:
            return {
                "success": False,
                "version": None,
                "error": "Version not found in output",
            }
        major = int(match.group(1))
        minor = int(match.group(2))

        # Find minor version
        # match = re.search(r"Aux Firmware Rev Info\s+:\n\s+0x(\d+)", result.stdout)
        match = re.search(r"Aux Firmware Rev Info\s+:\s+\n\s+0x(\d+)", result.stdout)
        if not match:
            return {
                "success": False,
                "version": None,
                "error": "Aux version not found in output",
            }
        aux = int(match.group(1))
        return {"success": True, "version": (major, minor, aux), "error": None}
    except subprocess.TimeoutExpired:
        return {"success": False, "version": None, "error": "Command timed out"}
    except Exception as e:
        return {"success": False, "version": None, "error": str(e)}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Galaxy Flash Utility", allow_abbrev=False
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    # Parser for integer that handles hex or dec
    def auto_int(x):
        return int(x, 0)

    # Write command
    write_parser = subparsers.add_parser("write", help="Write data to flash memory")
    write_parser.add_argument("ubb_id", type=int, help="UBB index (0-3)")
    write_parser.add_argument("asic_num", type=int, help="ASIC index (0-8)")
    write_parser.add_argument(
        "address", type=auto_int, help="Flash memory address to write to (e.g. 0x10000)"
    )
    write_parser.add_argument(
        "file", type=Path, help="Path to file to write (intel hex or binary supported)"
    )

    # Read command
    read_parser = subparsers.add_parser("read", help="Read data from flash memory")
    read_parser.add_argument("ubb_id", type=int, help="UBB index (0-3)")
    read_parser.add_argument("asic_num", type=int, help="ASIC index (0-8)")
    read_parser.add_argument(
        "address",
        type=auto_int,
        help="Flash memory address to read from (e.g. 0x10000)",
    )
    read_parser.add_argument(
        "length", type=auto_int, help="Number of bytes to read (e.g. 0x1000)"
    )
    read_parser.add_argument("file", type=Path, help="Path to output binary file")

    return parser.parse_args()


def main():
    args = parse_args()

    # Validate UBB and ASIC indices
    if not 0 <= args.ubb_id <= 3:
        print("Error: UBB index must be between 0 and 3")
        return
    if not 0 <= args.asic_num <= 8:
        print("Error: ASIC index must be between 0 and 8")
        return

    version = check_bmc_version()
    if not version["success"]:
        print(f"Error checking BMC version: {version['error']}")
        return
    if version["version"] < (0, 5, 14):
        print(f"Error, unsupported BMC version: {version['version']}")
        return

    if args.command == "write":
        if not args.file.is_file():
            print(f"Error: File {args.file} does not exist")
            return
        try:
            if (
                args.file.suffix.lower() == ".hex"
                or args.file.suffix.lower() == ".ihex"
            ):
                print(f"Programming from Intel Hex file: {args.file}")
                ih = IntelHex(str(args.file))
                write_flash(args.ubb_id, args.asic_num, ih)
            else:
                print(f"Programming binary file: {args.file}")
                ih = IntelHex()
                # Load binary at offset
                ih.loadbin(args.file, offset=args.address)
                write_flash(args.ubb_id, args.asic_num, ih)
        except RuntimeError as e:
            print(f"Error: {e}")
        except ValueError as e:
            print(f"Input error: {e}")
    elif args.command == "read":
        try:
            data = read_flash(
                args.ubb_id,
                args.asic_num,
                args.address,
                args.length,
                progress_print=True,
            )
            with open(args.file, "wb") as f:
                f.write(data)
        except RuntimeError as e:
            print(f"Error: {e}")
        except ValueError as e:
            print(f"Input error: {e}")


if __name__ == "__main__":
    main()
