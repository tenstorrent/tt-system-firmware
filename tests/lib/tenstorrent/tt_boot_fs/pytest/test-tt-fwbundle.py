# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import base64
from pathlib import Path
import logging
import pytest
from intelhex import IntelHex
import hashlib
import requests
import tempfile
import tarfile
import os
import sys

# Add to import path so we can import tt_fwbundle
TEST_ROOT = Path(__file__).parent.resolve()
MODULE_ROOT = TEST_ROOT.parents[4]
WORKSPACE_ROOT = MODULE_ROOT.parent
sys.path.append(str(MODULE_ROOT / "scripts"))
import tt_fwbundle  # noqa: E402

logging.basicConfig()
logger = logging.getLogger(__name__)

# Use a known good firmware bundle URL for testing (19.1.0)
FWBUNDLE_URL = "https://github.com/tenstorrent/tt-system-firmware/releases/download/v19.1.0/fw_pack-19.1.0.fwbundle"
FWBUNDLE_DIFF_URL = "https://github.com/tenstorrent/tt-system-firmware/releases/download/v19.1.0-rc1/fw_pack-19.1.0-rc1.fwbundle"
# The below constants are based on the known contents of the above firmware bundle
EXPECTED_P150A_MAINIMG_SHA256 = (
    "bcff42e138ac93bbf373619b57ef33f3c9c38f1e015c082668123d3d3cec5d6d"
)
EXPECTED_P150A_JSON = {
    "manifest": {
        "version": "2.0.0",
        "bundle_version": {"fwId": 19, "releaseId": 1, "patch": 0, "debug": 0},
    },
    "P150A-1": {
        "bootfs": {
            0: [
                {
                    "spi_addr": 81920,
                    "image_tag": "cmfw",
                    "size": 37824,
                    "copy_dest": 268435456,
                    "data_crc": 3812869019,
                    "flags": 33592256,
                    "fd_crc": 1823212734,
                    "digest": "N/A",
                },
                {
                    "spi_addr": 212992,
                    "image_tag": "safeimg",
                    "size": 107280,
                    "copy_dest": 0,
                    "data_crc": 3478886352,
                    "flags": 107280,
                    "fd_crc": 892225980,
                    "digest": "c02a20aaec523bf9cb81239e00852004290989706dd878eec114cda33b9f8e5c",
                },
                {
                    "spi_addr": 733184,
                    "image_tag": "safetail",
                    "size": 4096,
                    "copy_dest": 0,
                    "data_crc": 55547966,
                    "flags": 4096,
                    "fd_crc": 3576339237,
                    "digest": "N/A",
                },
                {
                    "spi_addr": 868352,
                    "image_tag": "boardcfg",
                    "size": 16,
                    "copy_dest": 0,
                    "data_crc": 2186797754,
                    "flags": 16,
                    "fd_crc": 1546450320,
                    "digest": "N/A",
                },
                {
                    "spi_addr": 872448,
                    "image_tag": "origcfg",
                    "size": 72,
                    "copy_dest": 0,
                    "data_crc": 3348568774,
                    "flags": 72,
                    "fd_crc": 796214240,
                    "digest": "N/A",
                },
                {
                    "spi_addr": 876544,
                    "image_tag": "bmfw",
                    "size": 45608,
                    "copy_dest": 0,
                    "data_crc": 3565595612,
                    "flags": 45608,
                    "fd_crc": 1274751846,
                    "digest": "ab588f97e5b83d83e57cfff4e903049d931a5e778bde8f5609c4a9d2b9d0150e",
                },
                {
                    "spi_addr": 946176,
                    "image_tag": "blupdate",
                    "size": 40460,
                    "copy_dest": 0,
                    "data_crc": 579834840,
                    "flags": 40460,
                    "fd_crc": 4169691050,
                    "digest": "N/A",
                },
                {
                    "spi_addr": 2060288,
                    "image_tag": "cmfwcfg",
                    "size": 72,
                    "copy_dest": 0,
                    "data_crc": 3348568774,
                    "flags": 72,
                    "fd_crc": 1065639636,
                    "digest": "N/A",
                },
                {
                    "spi_addr": 2064384,
                    "image_tag": "ethfwcfg",
                    "size": 512,
                    "copy_dest": 0,
                    "data_crc": 3211121,
                    "flags": 512,
                    "fd_crc": 3458160973,
                    "digest": "N/A",
                },
                {
                    "spi_addr": 2068480,
                    "image_tag": "memfwcfg",
                    "size": 256,
                    "copy_dest": 0,
                    "data_crc": 15951,
                    "flags": 256,
                    "fd_crc": 3455293491,
                    "digest": "N/A",
                },
                {
                    "spi_addr": 2072576,
                    "image_tag": "ethsdreg",
                    "size": 1152,
                    "copy_dest": 0,
                    "data_crc": 898158546,
                    "flags": 1152,
                    "fd_crc": 276192027,
                    "digest": "N/A",
                },
                {
                    "spi_addr": 2076672,
                    "image_tag": "flshinfo",
                    "size": 4,
                    "copy_dest": 0,
                    "data_crc": 50462976,
                    "flags": 4,
                    "fd_crc": 3673918419,
                    "digest": "N/A",
                },
                {
                    "spi_addr": 2080768,
                    "image_tag": "ethfw",
                    "size": 42732,
                    "copy_dest": 0,
                    "data_crc": 391411643,
                    "flags": 42732,
                    "fd_crc": 2111656835,
                    "digest": "N/A",
                },
                {
                    "spi_addr": 2146304,
                    "image_tag": "memfw",
                    "size": 13364,
                    "copy_dest": 0,
                    "data_crc": 30877635,
                    "flags": 13364,
                    "fd_crc": 1751482843,
                    "digest": "N/A",
                },
                {
                    "spi_addr": 2211840,
                    "image_tag": "ethsdfw",
                    "size": 19516,
                    "copy_dest": 0,
                    "data_crc": 3913070172,
                    "flags": 19516,
                    "fd_crc": 1564384609,
                    "digest": "N/A",
                },
                {
                    "spi_addr": 2281472,
                    "image_tag": "dmfw",
                    "size": 4096,
                    "copy_dest": 0,
                    "data_crc": 0,
                    "flags": 4096,
                    "fd_crc": 2005486948,
                    "digest": "N/A",
                },
                {
                    "spi_addr": 2285568,
                    "image_tag": "dmfwimg",
                    "size": 64920,
                    "copy_dest": 0,
                    "data_crc": 2409070098,
                    "flags": 64920,
                    "fd_crc": 126432887,
                    "digest": "d3c10cfac992cd02c299f7c8fccde2e39d02fefd74166df3fc1ffcf74cfa2925",
                },
                {
                    "spi_addr": 2740224,
                    "image_tag": "dmfwtail",
                    "size": 4096,
                    "copy_dest": 0,
                    "data_crc": 55548220,
                    "flags": 4096,
                    "fd_crc": 3880339476,
                    "digest": "N/A",
                },
                {
                    "spi_addr": 2744320,
                    "image_tag": "mainimg",
                    "size": 135684,
                    "copy_dest": 0,
                    "data_crc": 154060644,
                    "flags": 135684,
                    "fd_crc": 2016118846,
                    "digest": "3ddbc657b5fc87f3edb184bc5a5d88dec99d8e7b8e26d483100ec472f13dea54",
                },
                {
                    "spi_addr": 3264512,
                    "image_tag": "maintail",
                    "size": 4096,
                    "copy_dest": 0,
                    "data_crc": 55548220,
                    "flags": 4096,
                    "fd_crc": 3730062365,
                    "digest": "N/A",
                },
            ],
            0x4000: [
                {
                    "spi_addr": 737280,
                    "image_tag": "failover",
                    "size": 37824,
                    "copy_dest": 268435456,
                    "data_crc": 3812869019,
                    "flags": 33592256,
                    "fd_crc": 3558764336,
                    "digest": "N/A",
                },
            ],
        },
        "sha256": "ddf0c619c5144730f91517fecdaf7bf7715b7ad170ffa7ff3a3d5ab72ee050f4",
    },
}


@pytest.fixture()
def workdir() -> Path:
    with tempfile.TemporaryDirectory() as tmpdirname:
        yield Path(tmpdirname)


def download_fwbundle(out: Path, url: str):
    """
    Download a firmware bundle from a URL to the specified output path.
    """
    response = requests.get(url)
    response.raise_for_status()
    logger.info(f"Downloading firmware bundle from {url} to {out}")

    with open(out, "wb") as f:
        f.write(response.content)


def test_fwbundle_extract(workdir: Path):
    """
    Validate that we can extract a bootfs tag from a firmware bundle.
    """
    fwbundle_path = workdir / "fw_pack-19.1.0.fwbundle"
    download_fwbundle(fwbundle_path, FWBUNDLE_URL)
    out_path = workdir / "mainimg.bin"
    tt_fwbundle.extract_bundle_binary(fwbundle_path, "P150A-1", "mainimg", out_path)
    assert out_path.exists(), "Extracted mainimg binary does not exist"
    with open(out_path, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    assert digest == EXPECTED_P150A_MAINIMG_SHA256, (
        "Extracted mainimg binary SHA256 does not match expected value"
    )
    # Verify we can't extract a non-existent tag
    out_path = workdir / "nonexistent.bin"
    tt_fwbundle.extract_bundle_binary(
        fwbundle_path, "P150A-1", "nonexistenttag", out_path
    )
    assert not out_path.exists(), (
        "Non-existent tag extraction should not produce an output file"
    )


def test_fwbundle_create(workdir: Path):
    """
    Validate that we can create a firmware bundle from bootfs images.
    """
    fwbundle_path = workdir / "fw_pack-19.1.0.fwbundle"
    download_fwbundle(fwbundle_path, FWBUNDLE_URL)
    # Manually extract the image using tar
    bootfs_dict = {}
    os.mkdir(workdir / "extracted")
    with tarfile.open(fwbundle_path, "r") as tar:
        tar.extractall(path=workdir / "extracted", filter="data")
        images = [name for name in tar.getnames() if "image.bin" in name]
        for image in images:
            board = image.split("/")[1]
            image_path = workdir / "extracted" / image
            # Parse image path back to binary
            fname = f"{board}_bootfs.bin"
            ih = IntelHex()
            offset = 0
            with open(image_path, "r") as f:
                lines = f.readlines()
                for line in lines:
                    if line.startswith("@"):
                        # This is an address line, pad data to this point
                        offset = int(line[1:], 10)
                        fname = f"{board}_bootfs.hex"
                    else:
                        # This is a data line, decode and add
                        data = base64.b16decode(line.strip())
                        for byte in data:
                            ih[offset] = byte
                            offset += 1
            if fname.endswith(".bin"):
                image_out = workdir / "extracted" / fname
                ih.tobinfile(image_out)
            else:
                image_out = workdir / "extracted" / fname
                ih.tofile(image_out, format="hex")

            logger.debug(f"Bootfs image for board {board} extracted to {image_out}")
            bootfs_dict[board] = image_out
    # Create a new firmware bundle from the extracted images
    new_fwbundle_path = workdir / "fw_pack-new.fwbundle"
    logger.debug(f"Creating new firmware bundle at {new_fwbundle_path}")
    tt_fwbundle.create_fw_bundle(new_fwbundle_path, [19, 1, 0, 0], bootfs_dict)
    assert new_fwbundle_path.exists(), "Created firmware bundle does not exist"
    # New firmware bundle won't match original due to metadata differences,
    # but we can check all the files inside
    with (
        tarfile.open(fwbundle_path, "r") as orig_tar,
        tarfile.open(new_fwbundle_path, "r") as new_tar,
    ):
        orig_names = orig_tar.getnames()
        new_names = new_tar.getnames()
        assert orig_names == new_names, "Firmware bundle contents do not match"
        os.mkdir(workdir / "orig_temp")
        os.mkdir(workdir / "new_temp")
        orig_tar.extractall(path=workdir / "orig_temp", filter="data")
        new_tar.extractall(path=workdir / "new_temp", filter="data")
        for name in orig_names:
            orig_file = workdir / "orig_temp" / name
            new_file = workdir / "new_temp" / name
            if not orig_file.is_file():
                continue
            # NOTE: as a workaround for now, only compare blackhole boards.
            # Grayskull and Wormhole aren't reproducible using open source
            # tools
            if "P" not in name:
                continue
            d1 = hashlib.sha256(open(orig_file, "rb").read()).hexdigest()
            d2 = hashlib.sha256(open(new_file, "rb").read()).hexdigest()
            assert d1 == d2, (
                f"File {name} in firmware bundle does not match after re-creation"
            )


def test_fwbundle_diff(workdir: Path):
    """
    Validate that we can diff two firmware bundles.
    """
    fwbundle_path = workdir / "fw_pack-19.1.0.fwbundle"
    download_fwbundle(fwbundle_path, FWBUNDLE_URL)
    # Create a copy of the firmware bundle
    fwbundle_copy_path = workdir / "fw_pack-19.1.0-copy.fwbundle"
    with open(fwbundle_path, "rb") as src, open(fwbundle_copy_path, "wb") as dst:
        dst.write(src.read())
    # Diff should show no differences
    assert tt_fwbundle.diff_fw_bundles(fwbundle_path, fwbundle_copy_path) == os.EX_OK, (
        "Diff of identical bundles should show no differences"
    )
    # Download a different firmware bundle
    fwbundle_diff_path = workdir / "fw_pack-19.1.0-rc1.fwbundle"
    download_fwbundle(fwbundle_diff_path, FWBUNDLE_DIFF_URL)
    # Diff should show differences
    assert tt_fwbundle.diff_fw_bundles(fwbundle_path, fwbundle_diff_path) != os.EX_OK, (
        "Diff of different bundles should show differences"
    )


def test_bundle_metadata(workdir: Path):
    """
    Validate that the bundle metadata parser works
    """
    fwbundle_path = workdir / "fw_pack-19.1.0.fwbundle"
    download_fwbundle(fwbundle_path, FWBUNDLE_URL)
    json = tt_fwbundle.bundle_metadata(fwbundle_path, board="P150A-1")
    assert json == EXPECTED_P150A_JSON, (
        "Listed firmware bundle JSON does not match expected value"
    )


def test_combine_fwbundle(workdir: Path):
    """
    Validate that we can combine multiple firmware bundles.
    """
    fwbundle_path1 = workdir / "fw_pack-19.1.0.fwbundle"
    download_fwbundle(fwbundle_path1, FWBUNDLE_URL)
    fwbundle_path2 = workdir / "fw_pack-19.1.0-rc1.fwbundle"
    download_fwbundle(fwbundle_path2, FWBUNDLE_DIFF_URL)
    combined_fwbundle_path = workdir / "fw_pack-combined.fwbundle"
    tt_fwbundle.combine_fw_bundles(
        [fwbundle_path1, fwbundle_path2], combined_fwbundle_path
    )
    # Not much else we can check here, this isn't really a logical combination
    # of bundles. Just verify the output exists.
    assert combined_fwbundle_path.exists(), "Combined firmware bundle does not exist"


def compat_variables_bundle(path: Path, workdir: Path, jedec_id: str):
    """
    A bundle whose only variable is the permitted SPI EEPROM JEDEC ID.
    """
    import json

    image = workdir / "image.bin"
    if not image.exists():
        image.write_bytes(b"\xa5" * 64)

    compat_variables = workdir / f"compat-{jedec_id}.json"
    with open(compat_variables, "w") as f:
        json.dump(
            {
                "version": 1,
                "variables": [
                    {
                        "name": "SPI EEPROM",
                        "number": 0,
                        "formatter": "hex",
                        "source": {"type": "telemetry", "tag": 80},
                        "constraints": [{"eq": jedec_id}],
                    }
                ],
            },
            f,
        )

    tt_fwbundle.create_fw_bundle(
        path, [19, 1, 0, 0], {"P150A-1": image}, compat_variables
    )


def test_fwbundle_diff_compat_variables(workdir: Path):
    """
    Two bundles carrying the same image can still disagree about the hardware
    it may be written to, and that is a difference worth reporting: it decides
    whether the bundle may be flashed at all.
    """
    mt25 = workdir / "mt25.fwbundle"
    gd25 = workdir / "gd25.fwbundle"
    mt25_again = workdir / "mt25-again.fwbundle"
    compat_variables_bundle(mt25, workdir, "0x20bb20")
    compat_variables_bundle(gd25, workdir, "0xc8631a")
    compat_variables_bundle(mt25_again, workdir, "0x20bb20")

    # Same image, same constraints
    assert tt_fwbundle.diff_fw_bundles(mt25, mt25_again) == os.EX_OK, (
        "Bundles that agree should show no differences"
    )

    # Same image, different constraints
    assert tt_fwbundle.diff_fw_bundles(mt25, gd25) != os.EX_OK, (
        "A compatibility-only difference must be reported"
    )


def test_bundle_metadata_records_compat_variables(workdir: Path):
    """
    A bundle that predates board variables carries none, and must not grow the
    key.
    """
    with_compat = workdir / "with-compat.fwbundle"
    compat_variables_bundle(with_compat, workdir, "0x20bb20")
    metadata = tt_fwbundle.bundle_metadata(with_compat, board="P150A-1")
    assert metadata["P150A-1"]["compat_variables"]["variables"][0]["constraints"] == [
        {"eq": "0x20bb20"}
    ]

    without = workdir / "without-compat.fwbundle"
    tt_fwbundle.create_fw_bundle(
        without, [19, 1, 0, 0], {"P150A-1": workdir / "image.bin"}
    )
    assert (
        "compat_variables"
        not in tt_fwbundle.bundle_metadata(without, board="P150A-1")["P150A-1"]
    )
