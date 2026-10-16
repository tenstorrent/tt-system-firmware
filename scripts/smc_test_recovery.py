#!/usr/bin/env python3

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
This script performs recovery actions after a test failure. It is intended
to be run as part of a test framework's post-test hook. It dumps SMC state data
and resets the SMC. The script requires that the SMC be
accessible over PCIe, but does not require that the application firmware be in a
good state.
"""

import argparse

import dump_smc_state
import dmc_reset
import pcie_utils


def parse_args():
    """
    Parse command line arguments
    """
    parser = argparse.ArgumentParser(
        description="Perform recovery actions after a test failure", allow_abbrev=False
    )
    parser.add_argument(
        "--asic-id",
        type=int,
        default=0,
        help="ASIC ID of the target device (default: 0)",
    )
    return parser.parse_args()


def reset_dmc():
    """
    Reset the DMC, so the SMC will reboot
    """

    # Use dmc_reset module as a library to reset the DMC
    def args():
        return None

    args.config = dmc_reset.DEFAULT_DMC_CFG
    args.debug = 0
    args.openocd = dmc_reset.DEFAULT_OPENOCD
    args.scripts = dmc_reset.DEFAULT_SCRIPTS_DIR
    args.jtag_id = None
    args.hexfile = None
    args.timeout = 30

    dmc_reset.reset_dmc(args)


def recover_smc(asic_id):
    """
    Perform recovery actions on the SMC
    """
    # Dump SMC state
    try:
        dump_smc_state.dump_states(asic_id)
    except Exception as e:
        print(f"Failed to dump SMC state: {e}")
    # Reset DMC
    reset_dmc()
    # Rescan PCIe to ensure device is accessible
    pcie_utils.rescan_pcie()


def main():
    """
    Main function to perform recovery actions
    """
    args = parse_args()
    recover_smc(args.asic_id)


if __name__ == "__main__":
    main()
