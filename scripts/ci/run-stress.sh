#!/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025 Tenstorrent AI ULC

# This script is run within CI to execute the stress tests.
# It assumes the following:
# - The Zephyr SDK is installed and available in the PATH
# - The Zephyr base directory is set in the ZEPHYR_BASE environment variable
# - All necessary dependencies to build the firmware are installed
# - The DUT is connected and enumerating over PCIe

set -e # Exit on error

TT_Z_P_ROOT=$(realpath "$(dirname "$(realpath "$0")")"/../..)
# Prefer Zephyr base from environment, otherwise use the one in this repo
ZEPHYR_BASE=${ZEPHYR_BASE:-$(realpath "$TT_Z_P_ROOT"/../zephyr)}

function print_help {
	echo -n "Usage: $(basename "$0") [-p <pcie_index>] [-t test_set] <board_name> -- "
    echo "[additional twister args]"
	echo "Example: $(basename "$0") -p 0 -t e2e-stress p150a -- --clobber-output"
}

if [ $# -lt 1 ]; then
	print_help
	exit 1
fi

ASIC_ID=0

while getopts "p:t:h" opt; do
	case "$opt" in
		p) ASIC_ID=$OPTARG ;;
		t) TEST_SET=$TEST_SET:$OPTARG ;;
		h) print_help; exit 0 ;;
		\?) print_help; exit 1 ;;
	esac
done
shift $((OPTIND-1))
BOARD=$1
# Remove board argument and --
if [ $# -gt 1 ]; then
    shift 2
else
    shift
fi

CONSOLE_DEV="/dev/tenstorrent/$ASIC_ID"

# Export ASIC_ID, BOARD, and console dev as environment variables for use by scripts
export ASIC_ID
export CONSOLE_DEV
export BOARD

if [ -z "$TEST_SET" ]; then
    TEST_SET=":e2e-stress"
fi

echo "Using firmware root: $TT_Z_P_ROOT, Zephyr base: $ZEPHYR_BASE"
echo "Running stress tests on board: $BOARD, device: $CONSOLE_DEV, test set: ${TEST_SET}"
if [ $# -ne 0 ]; then
	echo "Additional twister args: $*"
fi

# Get SMC board name
SMC_BOARD=$("$TT_Z_P_ROOT"/scripts/rev2board.sh "$BOARD" smc)

# Start by building tt-console, so we can access the device
echo "Building tt-console..."
make -C "$TT_Z_P_ROOT"/scripts/tooling -j"$(nproc)"

if [[ "$TEST_SET" == *"e2e-stress"* ]]; then
	# Run the DMC tests
	echo "Running e2e stress tests..."
    # Reset the card first. If this fails tt-flash won't work either
    tt-smi -r
    # Run a full stress test, using tt-flash as the runner
    "$ZEPHYR_BASE/scripts/twister" -i -p "$SMC_BOARD" \
        --tag e2e-stress -T "$TT_Z_P_ROOT/app" \
        --west-flash="--force" \
        --west-runner tt_flash \
        --device-testing -c \
        --device-flash-timeout 120 \
        --pytest-args=--flash-timeout=120 \
        --device-serial-pty "$TT_Z_P_ROOT/scripts/smc_console.py -p -d $CONSOLE_DEV" \
        --flash-before \
        --outdir "$ZEPHYR_BASE/twister-e2e-stress" \
		-ll DEBUG \
        "$@"
fi
