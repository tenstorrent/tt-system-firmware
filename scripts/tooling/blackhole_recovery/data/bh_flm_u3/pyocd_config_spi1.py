# Copyright (c) 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

"""Register the GLX2 STM32U3 flash loader with pyOCD."""

from pathlib import Path
import sys

sys.path.append(str(Path(__file__).parent))
sys.path.append(str(Path(__file__).parent.parent / "common_flm"))
import pyocd_shared


def will_connect():
    """Register the GLX2 SPI flash region when pyOCD connects."""
    flm = Path(__file__).parent / "build" / "spi1_u3.flm"
    pyocd_shared.will_connect(flm, target)  # pylint: disable=undefined-variable # noqa: F821


def did_connect():
    """Install the custom SPI flash read hook after connection."""
    pyocd_shared.did_connect(target)  # pylint: disable=undefined-variable # noqa: F821
