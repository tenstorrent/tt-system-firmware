#!/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tenstorrent AI ULC

# This script is run within CI to execute the Nucleo hardware tests.
# It assumes the following:
# - The Zephyr SDK is installed and available in the PATH
# - The Zephyr base directory is set in the ZEPHYR_BASE environment variable
# - All necessary dependencies to build the firmware are installed
# - A nucleo_u385rg_q is connected over its onboard ST-Link
# - The board is wired for the i3c_loopback fixture:
#   - I3C1_SCL (PB2) <-> I3C2_SCL (PB10)
#   - I3C1_SDA (PC1) <-> I3C2_SDA (PB14)

set -e # Exit on error

TT_Z_P_ROOT=$(realpath "$(dirname "$(realpath "$0")")"/../..)
# Prefer Zephyr base from environment, otherwise use the one in this repo
ZEPHYR_BASE=${ZEPHYR_BASE:-$(realpath "$TT_Z_P_ROOT"/../zephyr)}

BOARD=nucleo_u385rg_q

function print_help {
	echo -n "Usage: $(basename "$0") -i <probe_id> -- "
	echo "[additional twister args]"
	echo "Example: $(basename "$0") -i 003900423234510337333934 -- --clobber-output"
	echo "Find the probe ID with 'pyocd list'"
}

while getopts "i:h" opt; do
	case "$opt" in
		i) PROBE_ID=$OPTARG ;;
		h) print_help; exit 0 ;;
		\?) print_help; exit 1 ;;
	esac
done
shift $((OPTIND-1))
# Remove --
if [ "$1" == "--" ]; then
	shift
fi

if [ -z "$PROBE_ID" ]; then
	print_help
	exit 1
fi

# The ST-Link exposes the board's UART as a virtual COM port. Use the by-id path
# so the right device is picked when several ST-Links are attached.
CONSOLE_DEV="/dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_${PROBE_ID}-if01"
if [ ! -e "$CONSOLE_DEV" ]; then
	echo "Console device $CONSOLE_DEV not found. Is the Nucleo connected?"
	exit 1
fi

echo "Using firmware root: $TT_Z_P_ROOT, Zephyr base: $ZEPHYR_BASE"
echo "Running Nucleo tests on board: $BOARD, probe: $PROBE_ID, console: $CONSOLE_DEV"
if [ $# -ne 0 ]; then
	echo "Additional twister args: $*"
fi

# Make sure we have STM32 target support
pyocd pack install stm32u385rgtxq

# Run the MCTP over I3C loopback tests
echo "Running MCTP I3C loopback tests..."
"$ZEPHYR_BASE"/scripts/twister -i \
	-p "$BOARD" --device-testing \
	--west-runner pyocd \
	--west-flash="--dev-id=$PROBE_ID" \
	--device-serial "$CONSOLE_DEV" \
	--fixture i3c_loopback \
	-T "$ZEPHYR_BASE/tests/subsys/pmci/mctp/i3c" \
	--outdir "$ZEPHYR_BASE/twister-nucleo" \
	"$@"
