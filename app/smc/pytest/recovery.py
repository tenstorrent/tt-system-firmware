#!/bin/env python3

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import ctypes
import hashlib
import logging
import pyluwen
import pytest
import subprocess
import sys
import tarfile
import time

from intelhex import IntelHex
from pathlib import Path
from twister_harness import DeviceAdapter

# Import tt_boot_fs utilities
sys.path.append(str(Path(__file__).parents[3] / "scripts"))

import tt_boot_fs
import tt_fwbundle

from pcie_utils import rescan_pcie
from tt_tools_common.reset_common.chip_reset import ChipReset

logger = logging.getLogger(__name__)

# Constant memory addresses we can read from SMC
ARC_POSTCODE_STATUS = 0x80030060
# Boot status register
ARC_BOOT_STATUS = 0x80030408

# Fixed descriptor table addresses used by the SMC ROM and generated bootfs.
ROM_TABLE_ADDR = 0x0
FAILOVER_TABLE_ADDR = 0x4000

# Boards with more than one ASIC build a bootfs per side, named for it.
ASIC_SIDES = ("left", "right")

# Where the driver exposes one node per PCIe-local ASIC.
TT_DEVICE_DIR = Path("/dev/tenstorrent")

# Chunk size for SPI reads, matching the pattern used by the e2e flash tests.
SPI_READ_CHUNK = 0x8000

# Repository root, used to locate the VERSION file.
REPO_ROOT = Path(__file__).parents[3]

# The last tt-flash release that predates the split ROM/mutable descriptor
# tables. It is what benches and developer machines already have installed, so
# a current bundle has to keep flashing with it.
LEGACY_TT_FLASH_VERSION = "3.10.0"


def firmware_bundle_version() -> list[int]:
    """
    Read the firmware bundle version from the repo VERSION file as
    [fwId, releaseId, patch, tweak]. tt-flash rejects bundles whose fwId is
    below the supported major release, so the patched bundle must carry the
    real version rather than a placeholder.
    """
    fields = {}
    for line in (REPO_ROOT / "VERSION").read_text().splitlines():
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        fields[key.strip()] = value.strip()
    return [
        int(fields.get("VERSION_MAJOR", 0) or 0),
        int(fields.get("VERSION_MINOR", 0) or 0),
        int(fields.get("PATCHLEVEL", 0) or 0),
        int(fields.get("VERSION_TWEAK", 0) or 0),
    ]


def pcie_asic_ids() -> list[int]:
    """
    PCI interface IDs of every ASIC the driver exposes.

    tt-flash has no way to address a single chip: it writes every PCIe-local
    board it detects. A corruption applied by flashing therefore lands on all of
    them, so anything done to undo it has to cover all of them too. On a p150
    that is one ASIC, on a p300 two, and on a galaxy every ASIC in the tray.
    """
    ids = sorted(
        int(node.name) for node in TT_DEVICE_DIR.iterdir() if node.name.isdigit()
    )
    assert ids, f"no Tenstorrent devices found under {TT_DEVICE_DIR}"
    return ids


def read_boot_status(asic_id: int = 0):
    """
    Helper to read the PCIe status register
    """
    chips = pyluwen.detect_chips()
    if len(chips) == 0:
        raise RuntimeError("PCIe card was not detected on this system")
    if asic_id >= len(chips):
        raise RuntimeError(f"ASIC {asic_id} was not detected")
    chip = chips[asic_id]
    try:
        status = chip.axi_read32(ARC_POSTCODE_STATUS)
    except Exception:
        print("Warning- SMC firmware requires a reset. Rescanning PCIe bus")
        rescan_pcie()
        status = chip.axi_read32(ARC_POSTCODE_STATUS)
    assert (status & 0xFFFF0000) == 0xC0DE0000, "SMC firmware postcode is invalid"
    # Check post code status of firmware
    assert (status & 0xFFFF) >= 0x1D, "SMC firmware boot failed"
    return chip.axi_read32(ARC_BOOT_STATUS)


def find_table(fs: tt_boot_fs.BootFs, offset: int) -> tt_boot_fs.BootTable:
    """Find a descriptor table by its absolute flash offset."""
    for table in fs.tables:
        if table.offset == offset:
            return table
    raise ValueError(f"bootfs table at 0x{offset:x} not found")


def find_entry(fs: tt_boot_fs.BootFs, tag: str) -> tt_boot_fs.FsEntry:
    """Find a uniquely named entry without depending on table ordering."""
    _, entry = fs.find_by_tag(tag)
    return entry


def all_bootfs_artifacts(build_dir: Path) -> list[tuple[Path, Path]]:
    """
    Return the bootfs HEX and YAML of every ASIC on the board, in ASIC ID order.
    """
    boot_fs = build_dir / "tt_boot_fs.hex"
    if boot_fs.exists():
        return [(boot_fs, build_dir / "tt_boot_fs.yaml")]

    return [
        (
            build_dir / f"tt_boot_fs-{side}.hex",
            build_dir / f"tt_boot_fs_{side}.yaml",
        )
        for side in ASIC_SIDES
    ]


def bundle_board_names(build_dir: Path) -> list[str]:
    """
    Return the board names the build's own bundle is keyed by, in ASIC ID order.

    tt-flash looks up each chip it detects by board name and aborts the update
    when one is missing, so a bundle assembled here has to use the same names.
    They are the product name the build hands to tt_fwbundle, which is not the
    name field in the bootfs YAML: on a galaxy those are GALAXY-3 and
    GALAXY_REVC-1 respectively. Read them back out of the bundle the build
    produced rather than deriving them a second, divergent way.
    """
    bundle = build_dir / "update.fwbundle"
    assert bundle.exists(), f"firmware bundle not found at {bundle}"
    with tarfile.open(bundle) as tar:
        names = {
            Path(member.name).parts[0]
            for member in tar.getmembers()
            if Path(member.name).parts and not member.name.endswith(".json")
        }

    if len(names) == 1:
        return list(names)

    # A multi-ASIC board carries one entry per side, suffixed by the build.
    ordered = []
    for side in ASIC_SIDES:
        matching = sorted(name for name in names if name.endswith(f"_{side}"))
        assert len(matching) == 1, (
            f"expected exactly one '{side}' entry in {bundle.name}, "
            f"found {matching} among {sorted(names)}"
        )
        ordered.append(matching[0])
    return ordered


def bootfs_artifacts(build_dir: Path, asic_id: int) -> tuple[Path, Path]:
    """Return the bootfs HEX and YAML corresponding to the selected ASIC."""
    artifacts = all_bootfs_artifacts(build_dir)
    if asic_id >= len(artifacts):
        raise ValueError(
            f"bootfs is only defined for ASIC IDs 0..{len(artifacts) - 1}, "
            f"got {asic_id}"
        )
    return artifacts[asic_id]


# Strategies for damaging the mutable main image (mainimg) so that MCUBoot
# rejects it and boots the recovery image instead. Signature and hash
# validation are disabled in this build (see mcuboot_hooks.c), so a corruption
# lands in recovery through one of two paths:
#
# 1. Header corruption -- MCUBoot's structural check runs before the hook, so a
#    bad magic word / header rejects the slot in boot_get_slot_usage and
#    find_slot_with_highest_version falls through to safeimg on the first boot.
# 2. Payload corruption behind a valid header -- MCUBoot loads and jumps to the
#    image because the hook skips content checks. The image never calls
#    boot_write_img_confirmed() (it crashed or hung), so on the *next* boot
#    boot_select_or_erase sees copy_done=SET && image_ok=UNSET and scrubs the
#    slot. safeimg is booted from the retry inside boot_load_and_validate_images.
#    That "next boot" doesn't happen on its own: neither the SMC hardware
#    watchdog nor the DMC auto-reset timer is armed at boot in this codebase
#    (both require an explicit TT_SMC_MSG_SET_WDT_TIMEOUT after boot). The test
#    uses ChipReset().full_lds_reset() to force that reset via the KMD's ASIC
#    reset ioctl -- the same primitive tt-flash uses at the end of a flash.
#
# Each strategy returns the bytes to write starting at the image's spi_addr.
def _corrupt_junk(_image: bytes) -> bytes:
    """Overlay non-MCUBoot ASCII on the header, e.g. from a stray write."""
    return b"BAD DATA"


def _corrupt_zeroed_header(_image: bytes) -> bytes:
    """Zero the whole header, as some flash failures leave cells at 0."""
    return bytes(32)


def _corrupt_erased_header(_image: bytes) -> bytes:
    """Set the header to 0xFF, matching an erase that finished but never programmed."""
    return b"\xff" * 32


def _corrupt_payload(image: bytes) -> bytes:
    """
    Keep the MCUBoot header intact and scramble the entire payload. MCUBoot's
    hook returns FIH_SUCCESS so the TLV hash isn't checked; the image is
    loaded to SRAM and jumped to. Overwriting the whole body guarantees the
    reset vector and any early handler is garbage regardless of what the
    linker put where.
    """
    hdr_size = 32
    if len(image) <= hdr_size:
        return image
    return bytes(image[:hdr_size]) + b"\xa5" * (len(image) - hdr_size)


MAINIMG_CORRUPTIONS = {
    "junk": _corrupt_junk,
    "zeroed_header": _corrupt_zeroed_header,
    "erased_header": _corrupt_erased_header,
    "payload": _corrupt_payload,
}

# Corruptions that leave the header intact require a second boot before the
# recovery image runs (see MAINIMG_CORRUPTIONS docstring for why).
NEEDS_EXTRA_RESET = {"payload"}


def make_corrupt_main_image_bundle(
    build_dir: Path, asic_id: int, corruption: str = "junk"
) -> tuple[Path, tt_boot_fs.BootFs]:
    """
    Build a firmware bundle identical to the one produced by the build, except
    with the mutable main image (mainimg) damaged in a specific way so it fails
    MCUBoot's structural checks. Returns the bundle path and the parsed boot
    filesystem.
    """
    boot_fs, _ = bootfs_artifacts(build_dir, asic_id)
    patched_fs = build_dir / f"tt_boot_fs_patched_{corruption}.hex"
    bundle = build_dir / f"tt_boot_fs_patched_{corruption}.bundle"
    assert boot_fs.exists(), f"bootfs HEX not found at {boot_fs}"
    # The bootfs is a sparse Intel HEX file. Decode it to locate entries by
    # their flash addresses.
    ih = IntelHex(str(boot_fs))
    fs = tt_boot_fs.BootFs.from_binary(bytes(ih.tobinarray(start=0)))
    mainimg = find_entry(fs, "mainimg")
    ih.frombytes(MAINIMG_CORRUPTIONS[corruption](mainimg.data), mainimg.spi_addr)
    ih.write_hex_file(str(patched_fs))
    # Make bundle from damaged mainimg. Use the real bundle version so tt-flash
    # accepts it; a placeholder version fails the manifest version check.
    #
    # The bundle has to describe every ASIC on the board, not just the one under
    # test: tt-flash looks up each chip it detects and aborts the whole update
    # when one is missing. On a P300 it would abort part way through, after
    # having already written the damaged image to the first ASIC.
    artifacts = all_bootfs_artifacts(build_dir)
    names = bundle_board_names(build_dir)
    assert len(names) == len(artifacts), (
        f"the build produced {len(artifacts)} bootfs images but its bundle is "
        f"keyed by {len(names)} board names ({names})"
    )
    images = {
        name: patched_fs if other_id == asic_id else other_hex
        for other_id, (name, (other_hex, _)) in enumerate(zip(names, artifacts))
    }
    tt_fwbundle.create_fw_bundle(bundle, firmware_bundle_version(), images)
    return bundle, fs


def flash_bundle(
    dut: DeviceAdapter,
    build_dir: Path,
    file: Path = None,
    tt_flash: Path = None,
    update_boot_images: bool = False,
):
    """
    Flash a firmware bundle with tt-flash. If file is None, the default bundle
    produced by the build is flashed. If tt_flash is None, whichever tt-flash
    the environment provides is used. Set update_boot_images to rewrite the
    boot-critical images that tt-flash would otherwise leave resident.
    """
    command = [
        dut.west,
        "flash",
        "--build-dir",
        str(build_dir),
        "--runner",
        "tt_flash",
        "--force",
        "--no-rebuild",
    ]
    if update_boot_images:
        command.append("--update-boot-images")
    if file is not None:
        command += ["--file", str(file)]
    if tt_flash is not None:
        command += ["--tt-flash", str(tt_flash)]
    dut.command = command
    dut._flash_and_run()


def read_spi(chip, addr: int, size: int) -> bytes:
    """
    Read size bytes from SPI starting at addr, in chunks, and return them.
    """
    bh = chip.as_bh()
    out = bytearray()
    offset = 0
    while offset < size:
        chunk = min(SPI_READ_CHUNK, size - offset)
        buf = bytes(chunk)
        bh.spi_read(addr + offset, buf)
        out += buf
        offset += chunk
    return bytes(out)


def boot_critical_digests(chip, fs: tt_boot_fs.BootFs) -> dict[str, str]:
    """
    Return sha256 digests of the boot-critical regions that a field update must
    not modify: the ROM and failover descriptor tables, plus the MCUBoot
    (cmfw), recovery (safeimg) and recovery-trailer (safetail) images the ROM
    table points at, and the failover image.
    """
    rom = find_table(fs, ROM_TABLE_ADDR)
    failover = find_table(fs, FAILOVER_TABLE_ADDR)

    # The descriptor region for a table starts at its header address. One flash
    # block comfortably covers the handful of descriptors in these tables.
    regions = {
        "rom_descriptors": (rom.offset, 0x1000),
        "failover_descriptors": (failover.offset, 0x1000),
    }
    # cmfw is MCUBoot; safeimg is the recovery image loaded by MCUBoot and
    # safetail the trailer that marks it bootable -- a recovery image without
    # its trailer is no more use than no recovery image at all. All three are
    # tagged by their device-tree partition labels in the generated bootfs.
    for tag in ("cmfw", "safeimg", "safetail"):
        entry = rom.entries[tag]
        regions[f"rom_{tag}"] = (entry.spi_addr, len(entry.data))
    failover_entry = failover.entries["failover"]
    regions["failover_image"] = (failover_entry.spi_addr, len(failover_entry.data))

    return {
        name: hashlib.sha256(read_spi(chip, addr, size)).hexdigest()
        for name, (addr, size) in regions.items()
    }


def mainimg_digest(chip, fs: tt_boot_fs.BootFs) -> str:
    """
    sha256 of the mutable main image on flash.

    The preservation assertions are all of the form "this did not change",
    which a test would also satisfy if it never managed to read the board or
    the flash never happened at all. This region is the control: the update
    under test deliberately changes it, so it must not come back equal.
    """
    entry = find_entry(fs, "mainimg")
    return hashlib.sha256(read_spi(chip, entry.spi_addr, len(entry.data))).hexdigest()


def diff_digests(before: dict[str, str], after: dict[str, str]) -> list[str]:
    """Return the names of regions whose digests changed."""
    return [name for name in before if before[name] != after.get(name)]


@pytest.mark.parametrize("corruption", sorted(MAINIMG_CORRUPTIONS))
def test_recovery_from_corrupted_mainimg(
    unlaunched_dut: DeviceAdapter, asic_id: int, corruption: str
):
    """
    Flash a bundle whose mainimg has been damaged in a specific way, confirm
    the SMC boots the recovery image, then flash a good bundle and confirm the
    SMC comes back on the main firmware. See MAINIMG_CORRUPTIONS for the range
    of damage covered and the two boot paths (header-reject and
    boot_select_or_erase after a hang) they exercise.
    """
    build_dir = unlaunched_dut.device_config.build_dir
    bad_bundle, _ = make_corrupt_main_image_bundle(build_dir, asic_id, corruption)

    flash_bundle(unlaunched_dut, build_dir, file=bad_bundle)
    if corruption in NEEDS_EXTRA_RESET:
        # MCUBoot's hook skips signature/hash validation, so the corrupted
        # payload loads and jumps. The image hangs before it can confirm itself,
        # and no watchdog is armed by default -- reset over the KMD's reset
        # ioctl (no JTAG needed; same primitive tt-flash uses) so
        # boot_select_or_erase runs on the next boot and scrubs the slot.
        #
        # Every ASIC needs this, not just the one under test. The flash above
        # damaged all of them, and an ASIC left hung in a jumped-to image has no
        # ARC to answer for it, so the reflash below would fail to identify the
        # board and abort before it could repair anything.
        time.sleep(2)
        ChipReset().full_lds_reset(pci_interfaces=pcie_asic_ids(), silent=True)
    time.sleep(1)
    assert (read_boot_status(asic_id) & 0x78) == 0x8, (
        f"Recovery firmware should be active after flashing '{corruption}' mainimg"
    )

    # Flash a good bundle back. Note- this requires an up to date version of tt-flash.
    flash_bundle(unlaunched_dut, build_dir)
    time.sleep(1)
    assert (read_boot_status(asic_id) & 0x78) == 0x0, (
        f"Main firmware should be running after reflashing a good bundle "
        f"(prior corruption: '{corruption}')"
    )


def test_rom_and_failover_preserved_on_update(
    unlaunched_dut: DeviceAdapter, asic_id: int
):
    """
    A field update must rewrite only the mutable table, leaving the boot-critical
    ROM (0x0) and failover (0x4000) tables untouched. This corrupts the mutable
    main image to force a recovery boot, then reflashes a good bundle, and
    verifies the ROM and failover descriptor tables and their boot-critical
    images (MCUBoot, recovery, recovery trailer) are byte-identical before the
    update, while booted in recovery, and after the update completes.

    Note what this can and cannot show. It compares content, so it catches a
    layout regression -- a record moving back into a static table, or a static
    record changing every release -- which would make tt-flash rewrite these
    regions. It cannot distinguish "tt-flash skipped this region" from
    "tt-flash erased it and wrote back identical bytes", because both leave the
    same digest. That the write plan omits them entirely is the subject of
    tt-flash's own unit tests (tests/test_boot_critical.py there).
    """
    build_dir = unlaunched_dut.device_config.build_dir
    bad_bundle, fs = make_corrupt_main_image_bundle(build_dir, asic_id)

    # Put the bench on the build under test first. These tests take an
    # unlaunched DUT, so twister's --flash-before never runs, and the
    # boot-critical images resident on the bench are whatever the last job left
    # there. Comparing those against the bundle would report a difference for
    # every image on any bench that is not already at this exact build.
    flash_bundle(unlaunched_dut, build_dir, update_boot_images=True)

    chip = pyluwen.detect_chips()[asic_id]
    before = boot_critical_digests(chip, fs)
    mainimg_before = mainimg_digest(chip, fs)

    # Flash the corrupted main image; the SMC should fall back to recovery.
    flash_bundle(unlaunched_dut, build_dir, file=bad_bundle)
    time.sleep(1)
    assert (read_boot_status(asic_id) & 0x78) == 0x8, (
        "Recovery firmware should be active"
    )

    # Even a flash that corrupted the main image and forced recovery must not
    # have disturbed the ROM or failover tables.
    chip = pyluwen.detect_chips()[asic_id]
    during = boot_critical_digests(chip, fs)
    assert during == before, (
        "ROM/failover tables changed after flashing a corrupted main image: "
        f"{diff_digests(before, during)}"
    )

    # The control for the assertions above: the update did reach the flash, and
    # these digests are of what is on the board rather than of nothing at all.
    assert mainimg_digest(chip, fs) != mainimg_before, (
        "the main image is unchanged after flashing a deliberately corrupted "
        "one, so the preservation checks above prove nothing"
    )

    # Flash a good image back and confirm normal boot resumes.
    flash_bundle(unlaunched_dut, build_dir)
    time.sleep(1)
    assert (read_boot_status(asic_id) & 0x78) == 0x0, (
        "Recovery firmware should no longer be active"
    )

    chip = pyluwen.detect_chips()[asic_id]
    after = boot_critical_digests(chip, fs)
    assert after == before, (
        "ROM/failover tables changed after reflashing a good image: "
        f"{diff_digests(before, after)}"
    )


def run_checked(command: list[str], what: str) -> subprocess.CompletedProcess:
    """Run a command, reporting its output rather than just a return code."""
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"failed to {what}: {detail}")
    return result


@pytest.fixture(scope="session")
def legacy_tt_flash(tmp_path_factory) -> Path:
    """
    Install the last released tt-flash into a throwaway virtualenv.

    It has to be isolated. This repo's environment tracks the tt-flash that
    ships alongside the firmware, and installing an older one over the top of
    it would change what every other test in the session flashes with.
    """
    venv_dir = tmp_path_factory.mktemp("legacy_tt_flash") / "venv"
    run_checked([sys.executable, "-m", "venv", str(venv_dir)], "create a virtualenv")
    run_checked(
        [
            str(venv_dir / "bin" / "pip"),
            "install",
            "--quiet",
            f"tt-flash=={LEGACY_TT_FLASH_VERSION}",
        ],
        f"install tt-flash {LEGACY_TT_FLASH_VERSION}",
    )
    binary = venv_dir / "bin" / "tt-flash"
    assert binary.exists(), (
        f"tt-flash {LEGACY_TT_FLASH_VERSION} installed, but {binary} is missing"
    )
    return binary


def read_bootfs_tables(chip) -> list[int]:
    """
    Return the descriptor table addresses advertised by the multi-table header
    on flash, having first checked the header is the one this firmware writes.
    """
    header_size = ctypes.sizeof(tt_boot_fs.tt_boot_fs_header)
    header = tt_boot_fs.tt_boot_fs_header.from_buffer_copy(
        read_spi(chip, tt_boot_fs.TT_BOOT_FS_HEADER_ADDR, header_size)
    )
    assert header.magic == tt_boot_fs.BOOTFS_HEADER_MAGIC, (
        f"bootfs header magic is {header.magic:#x}, expected "
        f"{tt_boot_fs.BOOTFS_HEADER_MAGIC:#x}"
    )
    assert header.version == tt_boot_fs.BOOTFS_VERSION, (
        f"bootfs header version is {header.version}, expected "
        f"{tt_boot_fs.BOOTFS_VERSION}"
    )
    # Check the count before using it as a length, so a corrupt header fails
    # here rather than sending a multi-megabyte read at the chip.
    assert 0 < header.num_tables <= tt_boot_fs.BOOTFS_TABLE_COUNT_MAX, (
        f"bootfs header advertises {header.num_tables} descriptor tables"
    )
    raw = read_spi(
        chip, tt_boot_fs.TT_BOOT_FS_HEADER_ADDR + header_size, 4 * header.num_tables
    )
    return [
        int.from_bytes(raw[i * 4 : (i + 1) * 4], "little")
        for i in range(header.num_tables)
    ]


def test_released_tt_flash_still_flashes(
    unlaunched_dut: DeviceAdapter, asic_id: int, legacy_tt_flash: Path
):
    """
    Flash a current bundle with the last released tt-flash and confirm the board
    comes back up with the split-table bootfs intact.

    Splitting the descriptor table changed the on-flash layout underneath a tool
    that is already installed on every bench. That tool writes each segment the
    bundle carries without interpreting the layout, so it still produces a
    working board. It rewrites the boot-critical images while doing so, which is
    what we had before the split -- this test covers "still flashes", not the
    skipping, which only the current tt-flash performs.

    The one place the released tool is not layout-agnostic is boardcfg: it
    preserves a board's provisioned configuration by reading that descriptor off
    the chip, and it only ever scans the table at 0x0. That is why boardcfg
    stays in the ROM table (see tt_blackhole_fixed_partitions.dtsi), and moving
    it is what this test catches -- the flash aborts with "Couldn't find
    boardcfg on chip" instead of failing quietly in the field.
    """
    build_dir = unlaunched_dut.device_config.build_dir
    boot_fs, _ = bootfs_artifacts(build_dir, asic_id)
    ih = IntelHex(str(boot_fs))
    fs = tt_boot_fs.BootFs.from_binary(bytes(ih.tobinarray(start=0)))
    expected_tables = sorted(table.offset for table in fs.tables)

    flash_bundle(unlaunched_dut, build_dir, tt_flash=legacy_tt_flash)
    time.sleep(1)

    assert (read_boot_status(asic_id) & 0x78) == 0x0, (
        "main firmware should be running after flashing with tt-flash "
        f"{LEGACY_TT_FLASH_VERSION}"
    )

    chip = pyluwen.detect_chips()[asic_id]
    assert sorted(read_bootfs_tables(chip)) == expected_tables, (
        "the descriptor tables on flash do not match the ones the bundle "
        f"defines after flashing with tt-flash {LEGACY_TT_FLASH_VERSION}"
    )

    # The board booting shows the ROM found a usable cmfw. Compare the images
    # themselves so a partial or misplaced write is caught too.
    for tag in ("cmfw", "safeimg", "safetail", "failover"):
        entry = find_entry(fs, tag)
        assert read_spi(chip, entry.spi_addr, len(entry.data)) == bytes(entry.data), (
            f"'{tag}' on flash does not match the bundle after flashing with "
            f"tt-flash {LEGACY_TT_FLASH_VERSION}"
        )
