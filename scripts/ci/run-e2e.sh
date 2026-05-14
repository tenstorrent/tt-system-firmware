#!/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025 Tenstorrent AI ULC

# This script is run within CI to execute E2E flash tests.
# It assumes the following:
# - The Zephyr SDK is installed and available in the PATH
# - The Zephyr base directory is set in the ZEPHYR_BASE environment variable
# - All necessary dependencies to build the firmware are installed
# - The DUT is connected and enumerating over PCIe

set -e # Exit on error

TT_Z_P_ROOT=$(realpath $(dirname $(realpath $0))/../..)
# Prefer Zephyr base from environment, otherwise use the one in this repo
ZEPHYR_BASE=${ZEPHYR_BASE:-$(realpath $TT_Z_P_ROOT/../zephyr)}

function print_help {
	echo -n "Usage: $0 [-p <pcie_index>] [-t test_set] [-k <keyfile>] "
	echo "<board_name> -- [additional twister args]"
	echo "Example: $0 -p 0 -t e2e-flash -k /tmp/test-key.pem p150a -- --clobber-output"
}

if [ $# -lt 1 ]; then
	print_help
	exit 1
fi

ASIC_ID=0

while getopts "k:p:t:h" opt; do
	case "$opt" in
		k) KEYFILE=$OPTARG ;;
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
    TEST_SET=":e2e-flash"
fi

if [ -z "$KEYFILE" ]; then
    # Generate a temporary key file that will be deleted on exit
    echo "No signing key file provided, generating temporary key"
    KEYFILE=$(mktemp)
    echo "Generating key file at $KEYFILE"
    trap 'rm -f "$KEYFILE"' EXIT
    openssl genrsa -out "$KEYFILE" 2048
fi

echo "Using firmware root: $TT_Z_P_ROOT, Zephyr base: $ZEPHYR_BASE"
echo "Running end-to-end tests on board: $BOARD, device: $CONSOLE_DEV, test set: ${TEST_SET}"
if [ $# -ne 0 ]; then
	echo "Additional twister args: $@"
fi

# Get SMC and DMC board names
SMC_BOARD=$($TT_Z_P_ROOT/scripts/rev2board.sh $BOARD smc)
DMC_BOARD=$($TT_Z_P_ROOT/scripts/rev2board.sh $BOARD dmc)

# Start by building tt-console, so we can access the device
echo "Building tt-console..."
make -C $TT_Z_P_ROOT/scripts/tooling -j$(nproc)

if [[ "$TEST_SET" == *"e2e-flash"* ]]; then
	# Run a full flash test, using tt-flash as the runner
	$ZEPHYR_BASE/scripts/twister -i -p $SMC_BOARD \
		--tag e2e-flash -T $TT_Z_P_ROOT/app \
		--west-flash="--force,--allow-major-downgrades" \
		--west-runner tt_flash \
		--device-testing -c \
		--device-flash-timeout 240 \
		--pytest-args=--flash-timeout=240 \
		--device-serial-pty "$TT_Z_P_ROOT/scripts/smc_console.py -d $CONSOLE_DEV -p" \
		--failure-script "$TT_Z_P_ROOT/scripts/smc_test_recovery.py --asic-id $ASIC_ID" \
		--flash-before \
		--outdir $ZEPHYR_BASE/twister-e2e-flash \
		--extra-args=SB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"$KEYFILE\" \
		-ll DEBUG \
		$@
	# Restore a stable DMFW, since the copy flashed by BL2 tests will
	# leave the DMC flash in a different state than other tests expect
	# We erase the DMC flash first to ensure no old image fragments remain
	west build -p always -b $DMC_BOARD $TT_Z_P_ROOT/app/dmc -d $ZEPHYR_BASE/build-dmc \
		--sysbuild
	west flash -d $ZEPHYR_BASE/build-dmc --domain mcuboot --erase \
		--cmd-erase 'flash erase_sector 0 0 last'
	west flash -d $ZEPHYR_BASE/build-dmc --domain dmc
	rm -rf $ZEPHYR_BASE/build-dmc
fi
