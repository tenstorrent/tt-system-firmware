# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

# Needed to keep ruff from complaining about this "unused import"
# ruff: noqa: F811
from e2e_smoke import arc_chip_dut  # noqa: F401


def test_boot_banner(arc_chip_dut):
    """
    Validates that the boot banner is printed correctly. This should run before
    any tests that reset the chip.
    """
    # Wait 3 seconds for console output
    output = arc_chip_dut.readlines_until(
        "Booting tt_blackhole with Zephyr OS", timeout=3000
    )
    out_str = "\n".join(output)
    assert "Booting tt_blackhole with Zephyr OS" in out_str
    print(f"\nconsole output successfully captured:\n{out_str}")
