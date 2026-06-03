# Copyright (c) 2024 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import logging
import os
import subprocess
import time

from pathlib import Path

TT_PCIE_VID = "0x1e52"

# ARC message IDs for ping
ARC_PING_MSG = 0x90
DMC_PING_MSG = 0xC0

# Common SMC register used for connectivity checks
SMC_POSTCODE_REG = 0x80030060

logger = logging.getLogger(Path(__file__).stem)

# Optional pyluwen import
try:
    import pyluwen

    HAS_PYLUWEN = True
except ImportError:
    HAS_PYLUWEN = False


def find_tt_devs():
    """
    Finds PCIe paths for devices to power off
    """
    devs = []
    for root, dirs, _ in os.walk("/sys/bus/pci/devices"):
        for d in dirs:
            with open(os.path.join(root, d, "vendor"), "r") as f:
                vid = f.read()
                if vid.strip() == TT_PCIE_VID:
                    devs.append(os.path.join(root, d))
    return devs


def rescan_pcie():
    """
    Helper to rescan PCIe bus
    """
    # First, we must find the PCIe card to power it off
    devs = find_tt_devs()
    for dev in devs:
        remove_path = Path(dev) / "remove"
        try:
            with open(remove_path, "w") as f:
                f.write("1")
        except PermissionError:
            try:
                subprocess.run(
                    ["sudo", "tee", str(remove_path)], input="1", text=True, stdout=subprocess.DEVNULL, check=True
                )
            except Exception as e:
                logger.error("this script must be run with elevated permissions")
                raise e

    # Now, rescan the bus
    rescan_path = Path("/sys/bus/pci/rescan")
    try:
        with open(rescan_path, "w") as f:
            f.write("1")
            time.sleep(1)
    except PermissionError:
        try:
            subprocess.run(["sudo", "tee", str(rescan_path)], input="1", text=True, stdout=subprocess.DEVNULL, check=True)
            time.sleep(1)
        except Exception as e:
            logger.error("this script must be run with elevated permissions")
            raise e


def require_pyluwen():
    """Raise an error if pyluwen is not available."""
    if not HAS_PYLUWEN:
        raise ImportError(
            "pyluwen is required but not installed. Install with: pip install pyluwen"
        )


def get_chip(asic_id=0, rescan_on_fail=True):
    """
    Get a pyluwen PciChip, optionally rescanning PCIe if the initial
    access returns 0xFFFFFFFF (device needs reset).
    """
    require_pyluwen()
    chip = pyluwen.PciChip(asic_id)
    try:
        postcode = chip.axi_read32(SMC_POSTCODE_REG)
        if postcode == 0xFFFFFFFF and rescan_on_fail:
            logger.info("SMC appears to need a PCIe reset, rescanning...")
            rescan_pcie()
            chip = pyluwen.PciChip(asic_id)
    except Exception:
        if rescan_on_fail:
            rescan_pcie()
            chip = pyluwen.PciChip(asic_id)
        else:
            raise
    return chip


def ping_arc(chip, asic_id=0):
    """Send ARC ping message. Returns True if ARC is responsive."""
    try:
        response = chip.arc_msg(ARC_PING_MSG, True, True, 0, 0)
        if response[0] == 1 and response[1] == 0:
            return True
        logger.error("ARC ping failed for ASIC %d: %s", asic_id, response)
        return False
    except BaseException as e:
        logger.error("Error pinging ARC for ASIC %d: %s", asic_id, e)
        return False


def ping_dmc(chip, asic_id=0, arg3=0):
    """Send DMC ping message. Returns True if DMC is responsive."""
    try:
        response = chip.arc_msg(DMC_PING_MSG, True, True, arg3, 0)
        if response[0] == 1 and response[1] == 0:
            return True
        logger.error("DMC ping failed for ASIC %d: %s", asic_id, response)
        return False
    except BaseException as e:
        logger.error("Error pinging DMC for ASIC %d: %s", asic_id, e)
        return False


def check_card_status(asic_id=0):
    """
    Check if a Tenstorrent card is enumerated and responds to
    ARC and DMC pings.

    Returns True if the card at the given asic_id responds to both pings.
    """
    if not Path(f"/dev/tenstorrent/{asic_id}").exists():
        logger.error("Card %d not found on bus", asic_id)
        return False

    require_pyluwen()
    try:
        chips = pyluwen.detect_chips()
        if asic_id >= len(chips):
            logger.error(
                "ASIC %d not found (only %d chips detected)", asic_id, len(chips)
            )
            return False
        card = chips[asic_id]

        if not ping_arc(card, asic_id):
            return False
        if not ping_dmc(card, asic_id):
            return False

        logger.info("Card %d: ARC and DMC pings OK", asic_id)
        return True

    except BaseException as e:
        logger.error("Error accessing card %d: %s", asic_id, e)
        return False


def wait_for_enum(asic_id=0, timeout=60, delay=2):
    """
    Wait for a card to enumerate on PCIe and become responsive.

    Repeatedly rescans the PCIe bus and checks card status until success
    or timeout.
    """
    deadline = time.time() + timeout

    while time.time() < deadline:
        try:
            rescan_pcie()
        except PermissionError:
            logger.error("Permission denied rescanning PCIe bus")
            return False

        if check_card_status(asic_id):
            return True

        remaining = deadline - time.time()
        logger.info("Card not ready, retrying... (%.0fs remaining)", remaining)
        time.sleep(delay)

    logger.error(
        "Timed out after %ds waiting for card %d to enumerate", timeout, asic_id
    )
    return False
