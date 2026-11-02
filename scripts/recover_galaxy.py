# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import pyluwen
from galaxy_flash import write_flash
from pcie_utils import rescan_pcie
import subprocess
import sys
import signal
import time
from intelhex import IntelHex

COORD_TO_I2C = {
    (0, 0): (0, 0),
    (3, 0): (1, 0),
    (0, 7): (2, 0),
    (3, 7): (3, 0),
    (0, 1): (0, 1),
    (3, 1): (1, 1),
    (0, 6): (2, 1),
    (3, 6): (3, 1),
    (0, 2): (0, 2),
    (3, 2): (1, 2),
    (0, 5): (2, 2),
    (3, 5): (3, 2),
    (0, 3): (0, 3),
    (3, 3): (1, 3),
    (0, 4): (2, 3),
    (3, 4): (3, 3),
    (1, 0): (0, 4),
    (2, 0): (1, 4),
    (1, 7): (2, 4),
    (2, 7): (3, 4),
    (1, 1): (0, 5),
    (2, 1): (1, 5),
    (1, 6): (2, 5),
    (2, 6): (3, 5),
    (1, 2): (0, 6),
    (2, 2): (1, 6),
    (1, 5): (2, 6),
    (2, 5): (3, 6),
    (1, 3): (0, 7),
    (2, 3): (1, 7),
    (1, 4): (2, 7),
    (2, 4): (3, 7),
}

UBB_MAP = {
    # UBB 1                         # UBB 2
    0xC1: (0, 7),
    0xC5: (1, 7),
    0x85: (2, 7),
    0x81: (3, 7),
    0xC2: (0, 6),
    0xC6: (1, 6),
    0x86: (2, 6),
    0x82: (3, 6),
    0xC3: (0, 5),
    0xC7: (1, 5),
    0x87: (2, 5),
    0x83: (3, 5),
    0xC4: (0, 4),
    0xC8: (1, 4),
    0x88: (2, 4),
    0x84: (3, 4),
    # UBB 3                         # UBB 4
    0x04: (0, 3),
    0x08: (1, 3),
    0x48: (2, 3),
    0x44: (3, 3),
    0x03: (0, 2),
    0x07: (1, 2),
    0x47: (2, 2),
    0x43: (3, 2),
    0x02: (0, 1),
    0x06: (1, 1),
    0x46: (2, 1),
    0x42: (3, 1),
    0x01: (0, 0),
    0x05: (1, 0),
    0x45: (2, 0),
    0x41: (3, 0),
}

TT_SMC_MSG_TEST = 0x90


def list_missing_asics():
    """
    Finds the I2C programming coordinates of all missing ASICs on the system.
    an ASIC is defined as "missing" if it fails to respond to a test ARC message, or
    is not present on the PCIe bus.
    """

    seen_coords = []
    missing_coords = []

    for i in range(32):
        try:
            chip = pyluwen.PciChip(i)
            response = chip.arc_msg(TT_SMC_MSG_TEST, True, False, 20, 0, 1000)
            if response[0] != 21:
                print(f"Chip {i} had unexpected response to test message")
        except Exception as e:
            print(f"Could not find chip {i}: {e}")
            continue
        pcie_bus = int(chip.get_pci_bdf()[5:7], 16)
        coords = UBB_MAP[pcie_bus]
        seen_coords.append(coords)
    for coord in UBB_MAP.values():
        if coord not in seen_coords:
            i2c_coord = COORD_TO_I2C[coord]
            missing_coords.append(i2c_coord)
    return missing_coords


def release_reset():
    print("Releasing ASIC reset...")
    subprocess.run("ipmitool raw 0x30 0x8b 0xf 0xff 0x2 0xf".split(), check=True)
    subprocess.run("ipmitool raw 0x30 0x8b 0xf 0xff 0x0 0xf".split(), check=True)
    print("Waiting 5s for ASICs to boot...")
    time.sleep(5)
    print("Rescanning PCIe bus...")
    rescan_pcie()


def sigint_handler(sig, frame):
    print("Caught SIGINT, clearing IPMI reset")
    release_reset()
    sys.exit(1)


def main():
    if len(sys.argv) != 2:
        print("Usage: recover_galaxy.py <firmware.hex>")
        sys.exit(1)
    ih = IntelHex()
    ih.loadhex(sys.argv[1])
    missing = list_missing_asics()
    if len(missing) == 0:
        print("All ASICs are alive, skipping programming")
        sys.exit(0)
    print(f"{len(missing)} ASIC(s) not alive")
    # Hold ASICs in reset while we program
    signal.signal(signal.SIGINT, sigint_handler)  # In case we are interrupted
    subprocess.run("ipmitool raw 0x30 0x8b 0xf 0xff 0x1 0xf".split(), check=True)
    for coord in missing:
        print(f"Missing coordinate {coord}")
        print("Programming Flash...")
        write_flash(coord[0], coord[1], ih)
    # Release reset
    release_reset()
    missing = list_missing_asics()
    if len(missing) > 0:
        print(f"Error, {len(missing)} asics still offline. Try a reboot?")
        sys.exit(1)

    # Check and set ASIC locations
    chips = pyluwen.detect_chips()
    for chip in chips:
        pcie_bus = int(chip.get_pci_bdf()[5:7], 16)
        expected = pcie_bus & 0xF
        table = chip.as_bh().decode_boot_fs_table("boardcfg")
        real = table["asic_location"]
        if expected != real:
            print(f"Chip location should be {expected}, it is {real}")
            table["asic_location"] = expected
            print("Writing new boardcfg with correct location...")
            chip.as_bh().encode_and_write_boot_fs_table(table, "boardcfg")

    print("You should now use tt-flash with --force to flash FW to ensure consistency")


if __name__ == "__main__":
    main()
