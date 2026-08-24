# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import argparse
import base64
import json
import jsonschema
import logging
import os
import pykwalify.core
import pytest
import requests
import sys
import tarfile
import types
import yaml

from pathlib import Path
from urllib.request import urlretrieve

TEST_ROOT = Path(__file__).parent.resolve()
MODULE_ROOT = TEST_ROOT.parents[4]
WORKSPACE_ROOT = MODULE_ROOT.parent
ZEPHYR_BASE = WORKSPACE_ROOT / "zephyr"

TEST_ALIGNMENT = 0x1000

sys.path.append(str(MODULE_ROOT / "scripts"))
sys.path.append(str(ZEPHYR_BASE / "scripts" / "dts" / "python-devicetree" / "src"))

import tt_boot_fs  # noqa: E402
import tt_fwbundle  # noqa: E402
import update_bar4_size  # noqa: E402
import fwtable_tooling  # noqa: E402
import update_tensix_disable_count  # noqa: E402

try:
    from yaml import CSafeLoader as SafeLoader
except ImportError:
    from yaml import SafeLoader

logger = logging.getLogger(__name__)

_cached_test_image = None
_cached_released_image = None
_cached_corrupted_test_image = None


def _align_up(val, alignment):
    return (val + alignment - 1) & ~(alignment - 1)


# @pytest.fixture(scope="session") ?
def gen_test_image(tmp_path: Path):
    global _cached_test_image
    if _cached_test_image is None:
        # Note: we don't (yet) use base64 encoding in this bin file, it's just binary data
        pth = tmp_path / "image.bin"
        spi_addr = tt_boot_fs.IMAGE_ADDR

        with open(pth, "wb") as f:
            pad_byte = b"\xff"

            # define 2 images
            image_A = b"\x73\x73\x42\x42"
            fd_A = tt_boot_fs.FsEntry(
                tag="imageA",
                data=image_A,
                load_addr=0x1000000,
                executable=True,
                provisioning_only=False,
                spi_addr=spi_addr,
            )
            spi_addr += len(image_A)
            spi_addr = _align_up(spi_addr, TEST_ALIGNMENT)

            image_B = b"\x73\x73\x42\x42\x37\x37\x24\x24"
            fd_B = tt_boot_fs.FsEntry(
                tag="imageB",
                data=image_B,
                provisioning_only=False,
                spi_addr=spi_addr,
                load_addr=None,
                executable=False,
            )
            spi_addr += len(image_A)
            spi_addr = _align_up(spi_addr, TEST_ALIGNMENT)

            # define 1 recovery image
            image_C = b"\x73\x73\x42\x42"
            fd_C = tt_boot_fs.FsEntry(
                tag="failover",
                data=image_C,
                spi_addr=spi_addr,
                load_addr=0x1000000,
                executable=False,
                provisioning_only=False,
            )

            # manually assemble a tt_boot_fs
            fs = b""

            # append file descriptors A and B
            fs += fd_A.descriptor()
            fs += fd_B.descriptor()

            # pad to FAILOVER_HEAD_ADDR
            pad_size = tt_boot_fs.FAILOVER_HEAD_ADDR - len(fs)
            padding = pad_byte * pad_size
            fs += padding

            # append recovery file descriptor (C)
            fs += fd_C.descriptor()

            # pad to SPI_RX_ADDR
            pad_size = tt_boot_fs.SPI_RX_ADDR - len(fs)
            padding = pad_byte * pad_size
            fs += padding

            # write SPI RX training data
            fs += tt_boot_fs.SPI_RX_VALUE.to_bytes(
                tt_boot_fs.SPI_RX_SIZE, byteorder="little"
            )

            # append images
            fs += image_A
            offs = len(fs)
            offs = _align_up(offs, TEST_ALIGNMENT)
            pad_size = offs - len(fs)
            padding = pad_byte * pad_size
            fs += padding

            fs += image_B
            offs = len(fs)
            offs = _align_up(offs, TEST_ALIGNMENT)
            pad_size = offs - len(fs)
            padding = pad_byte * pad_size
            fs += padding

            fs += image_C
            offs = len(fs)
            offs = _align_up(offs, TEST_ALIGNMENT)
            pad_size = offs - len(fs)
            padding = pad_byte * pad_size
            fs += padding

            f.write(fs)
            _cached_test_image = (fs, pth)

    return _cached_test_image


def get_test_image_path(tmp_path: Path):
    _, pth = gen_test_image(tmp_path)
    return pth


def gen_released_image(tmp_path: Path):
    global _cached_released_image

    if _cached_released_image is None:
        # Test on a recent experimental release (note: stable release does not have a recovery image)
        URL = (
            "https://github.com/tenstorrent/tt-firmware/raw/"
            "7bc0a90226e684962fb039cf26580356d7646574"
            "/fw_pack-80.15.0.0.fwbundle"
        )
        targz = tmp_path / "fw_pack.tar.gz"
        urlretrieve(URL, targz)

        with tarfile.open(targz, "r") as tar:
            tar.extractall(tmp_path / "fw_pack")

        with open(tmp_path / "fw_pack" / "P100A-1" / "image.bin", "r") as f:
            data = base64.b16decode(f.read())

        pth = tmp_path / "fw_pack" / "image.bin"

        with open(pth, "wb") as f:
            f.write(data)

        _cached_released_image = (data, pth)

    return _cached_released_image


def get_released_image_path(tmp_path: Path):
    _, pth = gen_released_image(tmp_path)
    return pth


def gen_corrupted_test_image(tmp_path: Path):
    global _cached_corrupted_test_image

    if _cached_corrupted_test_image is None:
        data, pth = gen_test_image(tmp_path)
        pth = Path(str(pth) + ".corrupted")

        data = bytearray(data)
        data[42] = 0x42
        data = bytes(data)

        with open(pth, "wb") as f:
            f.write(data)
            _cached_corrupted_test_image = (data, pth)

    return _cached_corrupted_test_image


def get_corrupted_test_image_path(tmp_path: Path):
    _, pth = gen_corrupted_test_image(tmp_path)
    return pth


def test_tt_boot_fs_schema():
    SCHEMA_PATH = MODULE_ROOT / "scripts" / "schemas" / "tt-boot-fs-schema.yml"
    SPEC_PATH = TEST_ROOT / "p100a.yml"

    schema = None
    spec = None
    with open(SCHEMA_PATH) as f:
        schema = yaml.load(f, Loader=SafeLoader)
    with open(SPEC_PATH) as f:
        spec = yaml.load(f, Loader=SafeLoader)
    spec = pykwalify.core.Core(source_data=spec, schema_data=schema).validate()


def test_tt_boot_fs_mkfs():
    """
    Test the ability to make a tt_boot_fs.
    """
    assert tt_boot_fs.mkfs(TEST_ROOT / "p100a.yml") is not None, (
        "tt_boot_fs.mkfs() failed"
    )


def test_tt_boot_fs_fsck(tmp_path: Path):
    """
    Test the ability to check a tt_boot_fs.
    """
    assert tt_boot_fs.fsck(get_test_image_path(tmp_path)), (
        "tt_boot_fs.fsck() failed with valid image"
    )
    assert not tt_boot_fs.fsck(get_corrupted_test_image_path(tmp_path)), (
        "tt_boot_fs.fsck() succeeded with invalid image"
    )

    assert tt_boot_fs.fsck(get_released_image_path(tmp_path)), (
        "tt_boot_fs.fsck() failed with released image"
    )


def test_tt_boot_fs_cksum():
    """
    Test the ability to generate a correct tt_boot_fs checksum.

    This test is intentionally consistent with the accompanying C ZTest in src/main.c.
    """

    items = [
        (0, []),
        (0, b"\x42"),
        (0x42427373, b"\x73\x73\x42\x42"),
        (0x6666AAAA, b"\x73\x73\x42\x42\x37\x37\x24\x24"),
    ]

    for it in items:
        assert tt_boot_fs.cksum(it[1]) == it[0]


def test_tt_boot_fs_ls(tmp_path: Path):
    """
    Test the ability to list a tt_boot_fs.
    """

    # Note: values are specific to fw_pack-80.15.0.0.fwbundle due to get_released_image_path()
    expected_fds = [
        {
            "spi_addr": 81920,
            "image_tag": "cmfwcfg",
            "size": 56,
            "copy_dest": 0,
            "data_crc": 2024482826,
            "digest": "N/A",
            "flags": 56,
            "fd_crc": 4034542600,
        },
        {
            "spi_addr": 86016,
            "image_tag": "cmfw",
            "size": 86600,
            "copy_dest": 268435456,
            "data_crc": 1374720981,
            "digest": "N/A",
            "flags": 33641032,
            "fd_crc": 3680084864,
        },
        {
            "spi_addr": 176128,
            "image_tag": "ethfwcfg",
            "size": 512,
            "copy_dest": 0,
            "data_crc": 2352493,
            "digest": "N/A",
            "flags": 512,
            "fd_crc": 3455414089,
        },
        {
            "spi_addr": 180224,
            "image_tag": "ethfw",
            "size": 34304,
            "copy_dest": 0,
            "data_crc": 433295191,
            "digest": "N/A",
            "flags": 34304,
            "fd_crc": 2151631411,
        },
        {
            "spi_addr": 217088,
            "image_tag": "memfwcfg",
            "size": 256,
            "copy_dest": 0,
            "data_crc": 15943,
            "digest": "N/A",
            "flags": 256,
            "fd_crc": 3453442091,
        },
        {
            "spi_addr": 221184,
            "image_tag": "memfw",
            "size": 10032,
            "copy_dest": 0,
            "data_crc": 3642299916,
            "digest": "N/A",
            "flags": 10032,
            "fd_crc": 1066009376,
        },
        {
            "spi_addr": 233472,
            "image_tag": "ethsdreg",
            "size": 1152,
            "copy_dest": 0,
            "data_crc": 897437643,
            "digest": "N/A",
            "flags": 1152,
            "fd_crc": 273632020,
        },
        {
            "spi_addr": 237568,
            "image_tag": "ethsdfw",
            "size": 19508,
            "copy_dest": 0,
            "data_crc": 3168980852,
            "digest": "N/A",
            "flags": 19508,
            "fd_crc": 818321009,
        },
        {
            "spi_addr": 258048,
            # Device Mgmt FW (called bmfw here for historical reasons)
            "image_tag": "bmfw",
            "size": 35704,
            "copy_dest": 0,
            "data_crc": 3947396359,
            "digest": "0ae8f44524478cd3a7fd278b9f87bdd3e49b153fee4adbb4f855774e3517f0e1",
            "flags": 35704,
            "fd_crc": 1655924193,
        },
        {
            "spi_addr": 294912,
            "image_tag": "flshinfo",
            "size": 4,
            "copy_dest": 0,
            "data_crc": 50462976,
            "digest": "N/A",
            "flags": 4,
            "fd_crc": 3672136659,
        },
        {
            "spi_addr": 299008,
            "image_tag": "failover",
            "size": 65828,
            "copy_dest": 268435456,
            "data_crc": 2239637331,
            "digest": "N/A",
            "flags": 33620260,
            "fd_crc": 1985122380,
        },
        {
            "spi_addr": 16773120,
            "image_tag": "boardcfg",
            "size": 0,
            "copy_dest": 0,
            "data_crc": 0,
            "digest": "N/A",
            "flags": 0,
            "fd_crc": 3670524614,
        },
    ]
    actual_fds = tt_boot_fs.ls(
        get_released_image_path(tmp_path),
        verbose=-2,
        output_json=True,
        input_base64=False,
    )
    assert actual_fds == expected_fds, "tt_boot_fs.ls() failed with valid image"

    assert not tt_boot_fs.ls(get_corrupted_test_image_path(tmp_path)), (
        "tt_boot_fs.ls() succeeded with invalid image"
    )


def test_tt_boot_fs_gen_yaml(tmp_path: Path):
    """
    Compares boot filesystem YAML generated from a devicetree to the existing hardcoded YAML files.
    Expects build/tt_boot_fs.yaml to already exist from sysbuild.
    """

    # Fetch expected YAML from v18.7.0 tag on GitHub
    expected_yaml_raw = "https://raw.githubusercontent.com/tenstorrent/tt-system-firmware/refs/tags/v18.7.0/boards/tenstorrent/tt_blackhole/bootfs/p150a-bootfs.yaml"
    response = requests.get(expected_yaml_raw)
    response.raise_for_status()
    expected_yaml = yaml.safe_load(response.text)

    # Generate YAML from bootfs
    tmp_path.mkdir(parents=True, exist_ok=True)

    dtsi = TEST_ROOT / "p150a-fixed-partitions.dts"

    args = argparse.Namespace(
        board="p150a",
        dts_file=dtsi,
        bindings_dirs=[MODULE_ROOT / "dts/bindings/", ZEPHYR_BASE / "dts/bindings/"],
        output_file=tmp_path / "tt_boot_fs.yaml",
        build_dir="$BUILD_DIR",
        blobs_dir="$ROOT/zephyr/blobs",
        verbose=None,
    )

    tt_boot_fs.invoke_generate_bootfs_yaml(args)
    with open(tmp_path / "tt_boot_fs.yaml", "r") as f:
        generated_yaml = yaml.safe_load(f)

    assert generated_yaml == expected_yaml, "Generated yaml differs from expected yaml"


def test_update_bar4_size(tmp_path: Path):
    """
    Tests that BAR4 can be resized by scripts/update_bar4_size.py.
    """

    version = "19.2.0"
    release_dl_url_base = (
        "https://github.com/tenstorrent/tt-system-firmware/releases/download/"
    )
    url = release_dl_url_base + f"v{version}/fw_pack-{version}.fwbundle"

    input_path = tmp_path / "input.fwbundle"
    output_path = input_path

    print(f"Downloading v{version} firmware bundle from {url}...")

    response = requests.get(url)
    response.raise_for_status()
    with open(input_path, "wb") as f:
        f.write(response.content)

    print("Updating BAR4 size for P100A-1 to 0 MiB...")

    cb_object = {
        "bus": [0],
        "size": 0,
    }

    assert os.EX_OK == fwtable_tooling.do_update(
        input_path,
        output_path,
        ["P100A-1"],
        update_bar4_size.iterate_bar4_sizes,
        cb_object,
        True,
    )


def test_tensix_disable(tmp_path: Path):
    """
    Tests that Tensix disable count can be updated by scripts/update_tensix_disable_count.py.
    """

    version = "19.2.0"
    release_dl_url_base = (
        "https://github.com/tenstorrent/tt-system-firmware/releases/download/"
    )
    url = release_dl_url_base + f"v{version}/fw_pack-{version}.fwbundle"

    input_path = tmp_path / "input.fwbundle"
    output_path = tmp_path / "output.fwbundle"

    print(f"Downloading v{version} firmware bundle from {url}...")

    response = requests.get(url)
    response.raise_for_status()
    with open(input_path, "wb") as f:
        f.write(response.content)

    print("Updating Tensix Disable Count for P150A-1 to 2...")

    cb_object = {
        "disable_count": 2,
    }

    assert os.EX_OK == fwtable_tooling.do_update(
        input_path,
        output_path,
        ["P150A-1"],
        update_tensix_disable_count.set_tensix_disable_count,
        cb_object,
        True,
    )

    assert tt_fwbundle.diff_fw_bundles(input_path, output_path) != os.EX_OK, (
        "diff_fw_bundles should detect changes after Tensix disable count update"
    )


def _flash_node(jedec_id: bytes = None, compats=("jedec,mspi-nor",), children=()):
    """
    The parts of an edtlib node the compat variables generator reads.
    """
    props = {}
    if jedec_id is not None:
        props["jedec-id"] = types.SimpleNamespace(val=jedec_id)
    if children:
        props["flash-devices"] = types.SimpleNamespace(val=list(children))
    return types.SimpleNamespace(compats=list(compats), props=props)


def _edt(flash_node):
    return types.SimpleNamespace(label2node={"spi_flash": flash_node})


def _compat_variables_validator():
    with open(tt_boot_fs.COMPAT_VARIABLES_SCHEMA_PATH) as f:
        schema = json.load(f)

    jsonschema.Draft202012Validator.check_schema(schema)
    return jsonschema.Draft202012Validator(schema)


def _compat_variables_doc(constraints: list, **overrides) -> dict:
    variable = {
        "name": "SPI EEPROM",
        "number": 0,
        "formatter": "hex",
        "source": {"type": "telemetry", "tag": 80},
        "constraints": constraints,
    }
    variable.update(overrides)
    return {"version": 1, "variables": [variable]}


def test_compat_variables_schema():
    """
    The generated compat variables must validate against the published schema.
    """
    validator = _compat_variables_validator()

    for flash in (
        # One part, several parts, and any part at all
        _flash_node(b"\x20\xbb\x20"),
        _flash_node(
            compats=("tenstorrent,flash-mux",),
            children=(_flash_node(b"\x20\xbb\x20"), _flash_node(b"\xc8\x63\x1a")),
        ),
        _flash_node(
            compats=("tenstorrent,flash-mux",),
            children=(_flash_node(b"\x20\xbb\x20"), _flash_node()),
        ),
    ):
        compat_variables = tt_boot_fs._compat_variables(_edt(flash))

        validator.validate(compat_variables)


@pytest.mark.parametrize(
    "constraints",
    [
        # Both representations of an unsigned 32 bit value, at both ends
        [{"eq": 0}],
        [{"eq": 4294967295}],
        [{"eq": "0x20bb20"}],
        [{"eq": "0X20BB20"}],
        [{"in": ["0x20bb20", 2145056]}],
        # No restriction at all
        [],
        # Every operator, and more than one at once
        [{"ne": 1}],
        [{"lt": 1}],
        [{"le": 1}],
        [{"gt": 1}],
        [{"ge": 1}],
        [{"not_in": [1]}],
        [{"ge": 2}, {"lt": 4}],
        # A list can repeat an operator, which a map could not
        [{"ne": 1}, {"ne": 2}],
    ],
)
def test_compat_variables_schema_accepts(constraints):
    _compat_variables_validator().validate(_compat_variables_doc(constraints))


@pytest.mark.parametrize(
    "constraints",
    [
        # Outside the range a board variable can hold
        [{"eq": -1}],
        [{"eq": 4294967296}],
        [{"eq": "0x100000000"}],
        # Not one of the two representations
        [{"eq": "20bb20"}],
        [{"eq": "nope"}],
        [{"eq": None}],
        [{"eq": True}],
        [{"eq": {"a": 1}}],
        [{"eq": [1]}],
        [{"in": [None]}],
        # A list of permitted values that permits nothing
        [{"in": []}],
        # An operator no consumer of version 1 knows how to evaluate
        [{"between": [1, 2]}],
        # An entry must name exactly one comparison
        [{"ge": 2, "lt": 4}],
        [{}],
        # The whole field is a list now, not a map
        {"eq": 0},
    ],
)
def test_compat_variables_schema_rejects(constraints):
    with pytest.raises(jsonschema.ValidationError):
        _compat_variables_validator().validate(_compat_variables_doc(constraints))


@pytest.mark.parametrize(
    "document",
    [
        # A misspelled key would otherwise be silently ignored
        _compat_variables_doc([{"eq": 0}], formater="hex"),
        _compat_variables_doc([{"eq": 0}], formatter="hexx"),
        _compat_variables_doc([{"eq": 0}], number=-1),
        # Past the last bit the unlock message can carry
        _compat_variables_doc([{"eq": 0}], number=224),
        _compat_variables_doc([{"eq": 0}], name=""),
        # A telemetry source with no tag names nothing to read
        _compat_variables_doc([{"eq": 0}], source={"type": "telemetry"}),
        _compat_variables_doc([{"eq": 0}], source={"type": "dmc_message"}),
        {"version": 1, "variables": [{"name": "x", "number": 0}]},
        {"version": 1},
        # This schema describes version 1 only
        {"version": 2, "variables": []},
    ],
)
def test_compat_variables_schema_rejects_structure(document):
    with pytest.raises(jsonschema.ValidationError):
        _compat_variables_validator().validate(document)


def test_compat_variables_schema_allows_the_last_representable_variable():
    """
    The verified bitmap is at most seven 32 bit words, so 223 is the highest
    number a host can ever report back.
    """
    _compat_variables_validator().validate(
        {"version": 1, "variables": [{"name": "x", "number": 223, "constraints": []}]}
    )


def test_compat_variables_schema_allows_an_unreadable_variable():
    """A variable with no source is one no consumer can check, not an error."""
    _compat_variables_validator().validate(
        {"version": 1, "variables": [{"name": "x", "number": 0, "constraints": []}]}
    )


def test_compat_variables_from_flash_mux():
    """
    A board that selects between flash parts at runtime supports all of them.
    """
    mux = _flash_node(
        compats=("tenstorrent,flash-mux",),
        children=(
            _flash_node(b"\x20\xbb\x20"),
            _flash_node(b"\xc2\x25\x3a"),
            _flash_node(b"\xc8\x63\x1a"),
            _flash_node(b"\xef\x60\x20"),
        ),
    )

    compat_variables = tt_boot_fs._compat_variables(_edt(mux))

    assert compat_variables == {
        "version": tt_boot_fs.COMPAT_VARIABLES_VERSION,
        "variables": [
            {
                "name": "SPI EEPROM",
                "number": tt_boot_fs.BOARD_VAR_SPI_JEDEC_ID,
                "formatter": "hex",
                "source": {"type": "telemetry", "tag": 80},
                "constraints": [
                    {"in": ["0x20bb20", "0xc2253a", "0xc8631a", "0xef6020"]}
                ],
            }
        ],
    }


def test_compat_variables_with_universal_fallback():
    """
    The mux can always fall back to the candidate that names no part, which
    drives universal commands and so accepts any chip. The image therefore
    supports any part, whatever the other candidates name.
    """
    mux = _flash_node(
        compats=("tenstorrent,flash-mux",),
        children=(
            _flash_node(b"\x20\xbb\x20"),
            # The universal single-lane fallback
            _flash_node(),
        ),
    )

    compat_variables = tt_boot_fs._compat_variables(_edt(mux))

    assert compat_variables["variables"][0]["constraints"] == []


def test_compat_variables_single_flash():
    """
    A board with one flash node supports only that part.
    """
    compat_variables = tt_boot_fs._compat_variables(_edt(_flash_node(b"\x20\xbb\x20")))

    assert compat_variables["variables"][0]["constraints"] == [{"eq": "0x20bb20"}]


def test_compat_variables_unknown_flash():
    """
    A lone flash node that names no part says nothing about which part its
    settings suit, so leave nothing declared rather than claim any part will
    do. Boards that fit exactly one part name it for this reason.
    """
    compat_variables = tt_boot_fs._compat_variables(_edt(_flash_node()))

    assert compat_variables["variables"] == []


def test_compat_variables_in_fwbundle(tmp_path: Path):
    """
    The generated file reaches the flashing tool in each board directory.
    """
    compat_variables = {
        "version": 1,
        "variables": [
            {
                "name": "SPI EEPROM",
                "number": 0,
                "formatter": "hex",
                "source": {"type": "telemetry", "tag": 80},
                "constraints": [{"eq": "0x20bb20"}],
            }
        ],
    }
    compat_variables_path = tmp_path / "compat-variables.json"
    with open(compat_variables_path, "w") as f:
        json.dump(compat_variables, f)

    bootfs = tmp_path / "tt_boot_fs.bin"
    with open(bootfs, "wb") as f:
        f.write(tt_boot_fs.mkfs(TEST_ROOT / "p100a.yml"))

    bundle = tmp_path / "update.fwbundle"
    tt_fwbundle.create_fw_bundle(
        bundle, [19, 1, 0, 0], {"P100A-1": bootfs}, compat_variables_path
    )

    with tarfile.open(bundle, "r") as tar:
        packed = json.load(tar.extractfile("./P100A-1/compat-variables.json"))

    assert packed == compat_variables

    # Bundles built without compat variables must not gain the file
    plain = tmp_path / "plain.fwbundle"
    tt_fwbundle.create_fw_bundle(plain, [19, 1, 0, 0], {"P100A-1": bootfs})
    with tarfile.open(plain, "r") as tar:
        assert "./P100A-1/compat-variables.json" not in tar.getnames()
