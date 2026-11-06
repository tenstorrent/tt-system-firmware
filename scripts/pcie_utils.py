# Copyright (c) 2024 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import logging
import os
import re
import subprocess
import time

from pathlib import Path
from typing import Optional

TT_PCIE_VID = "0x1e52"

# ARC message IDs for ping
ARC_PING_MSG = 0x90
DMC_PING_MSG = 0xC0

# Common SMC register used for connectivity checks
SMC_POSTCODE_REG = 0x80030060

# lspci LnkSta speed for each PCIe generation
PCIE_GEN_SPEED = {
    1: "2.5GT/s",
    2: "5GT/s",
    3: "8GT/s",
    4: "16GT/s",
    5: "32GT/s",
}

# Link width expected when none is given
PCIE_DEFAULT_WIDTH = 16

# Expected PCIe link per board: the generation, and the link widths that are
# accepted. "widths_behind_bus" overrides expected "widths" for certain bdfs
BOARD_PCIE_EXPECTATIONS = {
    "bh-galaxy-revc": {
        "gen": 5,
        "widths": (1,),
        "widths_behind_bus": {"0000:06": (8,)},
    },
    "quietbox2": {"gen": 4, "widths": (4, 8)},
    "loudbox": {"gen": 5, "widths": (16,)},
}

# lspci fields that check_link reads.
LSPCI_FIELDS = ("LnkSta", "DevSta", "CESta", "UESta", "LaneErrStat")

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


def rescan_pcie(remove=True):
    """
    Helper to rescan PCIe bus
    @param remove: unbind the cards first, so the rescan re-enumerates them
    """
    # First, we must find the PCIe card to power it off
    devs = find_tt_devs() if remove else []
    for dev in devs:
        remove_path = Path(dev) / "remove"
        try:
            with open(remove_path, "w") as f:
                f.write("1")
        except PermissionError:
            try:
                subprocess.call(
                    f"echo 1 | sudo tee {remove_path} > /dev/null", shell=True
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
            subprocess.call(f"echo 1 | sudo tee {rescan_path} > /dev/null", shell=True)
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


def check_link(
    bdf: str,
    lspci_out: str,
    gen: int,
    widths: tuple[int, ...] = (PCIE_DEFAULT_WIDTH,),
) -> tuple[bool, list[str]]:
    """
    Check the PCIe link of one device.

    @param bdf: PCI address of the device, for example "0000:01:00.0"
    @param lspci_out: output of `lspci -s <bdf> -vvv`
    @param gen: expected PCIe generation, 1 to 5
    @param widths: link widths that are accepted
    Returns (ok, report lines). A wrong link speed or width is not ok.
    Set error bits only add a warning to the report.
    """
    fields = {}
    for name in LSPCI_FIELDS:
        match = re.search(rf"^\s*{name}:.*$", lspci_out, re.MULTILINE)
        fields[name] = match.group(0).strip() if match else None

    lnk_sta = fields["LnkSta"]
    if lnk_sta is None:
        # No LnkSta usually means lspci could not read the extended config
        # space, so show the uid and effective capabilities to help debug that.
        cap_eff = ""
        try:
            with open("/proc/self/status") as f:
                cap_eff = next(l.split()[1] for l in f if l.startswith("CapEff"))
        except (OSError, StopIteration):
            pass
        return False, [
            f"ERROR: no LnkSta for {bdf} (id: {os.getuid()}, caps: {cap_eff})."
            " Raw output:",
            lspci_out,
        ]

    lines = [f"BusID: {bdf}", lnk_sta]
    speed = f"Speed {PCIE_GEN_SPEED[gen]}"
    if speed not in lnk_sta or not any(
        re.search(rf"Width x{w}\b", lnk_sta) for w in widths
    ):
        expected = " or ".join(f"Width x{w}" for w in widths)
        lines.append(f"ERROR: Observed: {lnk_sta}  Expected: {speed}  {expected}")
        return False, lines
    lines.append("Correct values")

    for name in ("DevSta", "CESta", "UESta"):
        lines.append(fields[name] or f"{name}: not reported")
        if fields[name] and "+" in fields[name]:
            lines.append("WARNING: Should be all negative(-)")
    lane_err = fields["LaneErrStat"]
    if lane_err is not None:
        lines.append(lane_err)
        if lane_err.split(":", 1)[1].strip() != "0":
            lines.append("WARNING: Should be 0")
    return True, lines


def check_all_pcie(
    expect: dict, bdf: Optional[str] = None, log_dir: str = "logs"
) -> bool:
    """
    Check the PCIe link of every Tenstorrent device, or only of bdf.

    The function checks all devices, also after a failure. It writes the
    report of each device to <log_dir>/pci-width-<bdf>.log.

    @param expect: expected link, in the format of BOARD_PCIE_EXPECTATIONS
    @param bdf: PCI address of one device. If None, check all devices.
    @param log_dir: directory for the report files
    Returns True only if every device passed.
    """
    bdfs = [bdf] if bdf else sorted(Path(d).name for d in find_tt_devs())
    if not bdfs:
        print("No Tenstorrent devices found")
        return False

    Path(log_dir).mkdir(parents=True, exist_ok=True)
    all_ok = True
    for dev in bdfs:
        widths = expect["widths"]
        sysfs_path = os.path.realpath(f"/sys/bus/pci/devices/{dev}")
        for bus, bus_widths in expect.get("widths_behind_bus", {}).items():
            if f"/{bus}:" in sysfs_path:
                widths = bus_widths

        # Keep stderr so sudo/lspci failures are visible in CI logs
        lspci_out = subprocess.run(
            ["sudo", "lspci", "-s", dev, "-vvv"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        ).stdout
        ok, lines = check_link(dev, lspci_out, expect["gen"], widths)
        if not ok:
            lines.append(f"{dev} FAILED")
            all_ok = False

        report = "\n".join(lines)
        print(f"::group::{dev}\n{report}\n::endgroup::")
        log = Path(log_dir) / f"pci-width-{dev.replace(':', '_')}.log"
        log.write_text(report + "\n")
    return all_ok
