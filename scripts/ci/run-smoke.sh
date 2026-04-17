#!/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025 Tenstorrent AI ULC

# This script is run within CI to execute the smoke tests.
# It assumes the following:
# - The Zephyr SDK is installed and available in the PATH
# - The Zephyr base directory is set in the ZEPHYR_BASE environment variable
# - All necessary dependencies to build the firmware are installed
# - The DUT is connected and enumerating over PCIe
# - An ST-Link adapter is connected to the DUT to flash firmware

set -e # Exit on error

TT_Z_P_ROOT=$(realpath "$(dirname "$(realpath "$0")")"/../..)
# Prefer Zephyr base from environment, otherwise use the one in this repo
ZEPHYR_BASE=${ZEPHYR_BASE:-$(realpath "$TT_Z_P_ROOT"/../zephyr)}

function print_help {
	echo -n "Usage: $(basename "$0") [-p <pcie_index>] [-t test_set] <board_name> -- "
	echo "[additional twister args]"
	echo "Example: $(basename "$0") -p 0 -t dmc -t smc p150a -- --clobber-output"
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
    TEST_SET=":dmc:smc"
fi

echo "Using firmware root: $TT_Z_P_ROOT, Zephyr base: $ZEPHYR_BASE"
echo "Running smoke tests on board: $BOARD, device: $CONSOLE_DEV, test set: ${TEST_SET}"
if [ $# -ne 0 ]; then
	echo "Additional twister args: $*"
fi

# Get SMC and DMC board names
SMC_BOARD=$("$TT_Z_P_ROOT"/scripts/rev2board.sh "$BOARD" smc)
DMC_BOARD=$("$TT_Z_P_ROOT"/scripts/rev2board.sh "$BOARD" dmc)

# Start by building tt-console, so we can access the device
echo "Building tt-console..."
make -C "$TT_Z_P_ROOT"/scripts/tooling -j"$(nproc)"

# Make sure we have STM32 target support
pyocd pack install stm32g0b1ceux

# Build the tt-bootstrap flash loading algorithms
echo "Building tt-bootstrap flash loading algorithms..."
"$TT_Z_P_ROOT"/scripts/tooling/blackhole_recovery/data/bh_flm/build-flm.sh

if [[ "$TEST_SET" == *"dmc"* ]]; then
	# Run the DMC tests
	echo "Running DMC tests..."
	# Run tests tagged with "smoke"
	"$ZEPHYR_BASE"/scripts/twister -i \
		-p "$DMC_BOARD" --device-testing \
		--west-flash \
		--device-serial-pty "$TT_Z_P_ROOT/scripts/dmc_rtt.py" \
		--flash-before \
		--tag smoke \
		--alt-config-root "$TT_Z_P_ROOT/test-conf/samples" \
		--alt-config-root "$TT_Z_P_ROOT/test-conf/tests" \
		-T "$ZEPHYR_BASE/samples" -T "$ZEPHYR_BASE/tests" -T "$TT_Z_P_ROOT/tests" \
		-T "$TT_Z_P_ROOT/samples" \
		--outdir "$ZEPHYR_BASE/twister-dmc-smoke" \
		"$@"
fi

if [[ "$TEST_SET" == *"smc"* ]]; then
	# Run the SMC tests
	echo "Running SMC tests..."
	# Flash the DMFW app back onto the DMC. Otherwise the flash device
	# will not be muxed to the SMC, and flash tests will fail
	"$ZEPHYR_BASE/scripts/twister" -i \
		--tag e2e \
		-p "$DMC_BOARD" --device-testing \
		--device-serial-pty "$TT_Z_P_ROOT/scripts/dmc_rtt.py" \
		--west-flash \
		--flash-before \
		-T "$TT_Z_P_ROOT/app" \
		--outdir "$ZEPHYR_BASE/twister-dmc-e2e" \
		"$@"

	  # Run tests tagged with "smoke"
	"$ZEPHYR_BASE/scripts/twister" -i \
		-p "$SMC_BOARD" --device-testing \
		--device-serial-pty "$TT_Z_P_ROOT/scripts/smc_console.py -d $CONSOLE_DEV" \
		--failure-script "$TT_Z_P_ROOT/scripts/smc_test_recovery.py" \
		--flash-before \
		--west-flash="--no-prompt" \
		--west-runner tt_bootstrap \
		--device-flash-timeout 120 \
		--tag smoke \
		--alt-config-root "$TT_Z_P_ROOT/test-conf/samples" \
		--alt-config-root "$TT_Z_P_ROOT/test-conf/tests" \
		-T "$ZEPHYR_BASE/samples" -T "$ZEPHYR_BASE/tests" \
		-T "$TT_Z_P_ROOT/tests" -T "$TT_Z_P_ROOT/samples" \
		--outdir "$ZEPHYR_BASE/twister-smc-smoke" \
		"$@"
fi
