#!/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tenstorrent AI ULC

# This script runs the manufacturing test sequence for Blackhole boards.
# It assumes the following:
# - pyocd is installed (with the STM32G0B1CEUx CMSIS pack) for early SPI preflash
#   (step 1) and assembly DMC flash (step 2)
# - The DUT is reachable via JTAG for pyocd steps and enumerated on PCIe for step 5
# - All required firmware artifacts are available in the specified directories

set -e

TT_Z_P_ROOT=$(realpath "$(dirname "$(realpath "$0")")"/../..)

# STM32 DMC target used by pyocd for both the SPI EEPROM (via the FLM user-script)
# and the DMC internal flash. Keep in sync with PYOCD_TARGET_BH in pyocd_utils.py.
PYOCD_TARGET="STM32G0B1CEUx"

function print_help {
	echo "Usage: $(basename "$0") [options] <board_name>"
	echo ""
	echo "Run the manufacturing test sequence for a Blackhole board."
	echo ""
	echo "Options:"
	echo "  -p <dir>   Directory containing the preflash ihex (default: artifacts/preflash)"
	echo "  -m <dir>   Directory containing assembly MCUBoot ELF"
	echo "             (default: artifacts/assembly-mcuboot)"
	echo "  -a <dir>   Directory containing assembly app hex (default: artifacts/assembly-fw)"
	echo "  -f <dir>   Directory containing production fwbundle (default: artifacts/fwbundle)"
	echo "  -h         Show this help"
}

if [ $# -lt 1 ]; then
	print_help
	exit 1
fi

PREFLASH_DIR="artifacts/preflash"
MCUBOOT_DIR="artifacts/assembly-mcuboot"
ASSEMBLY_FW_DIR="artifacts/assembly-fw"
FWBUNDLE_DIR="artifacts/fwbundle"

while getopts "p:m:a:f:h" opt; do
	case "$opt" in
		p) PREFLASH_DIR=$OPTARG ;;
		m) MCUBOOT_DIR=$OPTARG ;;
		a) ASSEMBLY_FW_DIR=$OPTARG ;;
		f) FWBUNDLE_DIR=$OPTARG ;;
		h) print_help; exit 0 ;;
		\?) print_help; exit 1 ;;
	esac
done
shift $((OPTIND-1))

BOARD=$1

if [ -z "$BOARD" ]; then
	echo "ERROR: Board name is required"
	print_help
	exit 1
fi

# Derive board properties (ASSEMBLY_BOARD, NUM_ASICS, PREFLASH_REV)
source "$TT_Z_P_ROOT/scripts/ci/board-map.sh"

echo "Board: $BOARD, Assembly board: $ASSEMBLY_BOARD, Num ASICs: $NUM_ASICS"
echo "Preflash rev: $PREFLASH_REV"
echo "Preflash dir: $PREFLASH_DIR"
echo "MCUBoot dir: $MCUBOOT_DIR"
echo "Assembly FW dir: $ASSEMBLY_FW_DIR"
echo "FW bundle dir: $FWBUNDLE_DIR"

function verify_pcie_enumeration {
	local DESCRIPTION=$1
	echo "Verifying PCIe enumeration ($DESCRIPTION)..."
	local TIMEOUT=60
	local ELAPSED=0
	while true; do
		echo "Rescanning PCIe bus..."
		"$TT_Z_P_ROOT"/scripts/rescan-pcie.sh
		local COUNT=0
		for dev in /sys/bus/pci/devices/*/vendor; do
			if [ -f "$dev" ] && [ "$(cat "$dev")" = "0x1e52" ]; then
				COUNT=$((COUNT + 1))
			fi
		done
		echo "Detected $COUNT Tenstorrent PCIe endpoint(s), expected $NUM_ASICS"
		if [ "$COUNT" -ge "$NUM_ASICS" ]; then
			echo "PASS: All $NUM_ASICS endpoint(s) enumerated"
			return 0
		fi
		if [ "$ELAPSED" -ge "$TIMEOUT" ]; then
			echo "FAIL: Only $COUNT of $NUM_ASICS endpoint(s)" \
				"enumerated after ${TIMEOUT}s"
			return 1
		fi
		echo "Retrying in 2 seconds..."
		sleep 2
		ELAPSED=$((ELAPSED + 2))
	done
}

# ---- Step 1: Erase SPI + flash preflash ihex ----
echo "=== Step 1: Erase SPI and flash preflash ihex ==="
PREFLASH_MATCHES=("$PREFLASH_DIR"/preflash-*.ihex)
if [ ! -e "${PREFLASH_MATCHES[0]}" ]; then
	echo "ERROR: No preflash .ihex found in $PREFLASH_DIR"
	exit 1
fi
PREFLASH_IHEX="${PREFLASH_MATCHES[0]}"
echo "Using preflash image: $PREFLASH_IHEX"
FLM_DIR="$TT_Z_P_ROOT/scripts/tooling/blackhole_recovery/data/bh_flm"
for cfg in "${PYOCD_CONFIGS[@]}"; do
	echo "Erasing SPI and writing preflash via $cfg ..."
	python3 -m pyocd flash -t "$PYOCD_TARGET" -O user_script="$FLM_DIR/$cfg" \
		--erase chip --format hex "$PREFLASH_IHEX"
done

# ---- Step 2: Flash assembly test FW to DMC via pyocd ----
echo "=== Step 2: Flash assembly test firmware to DMC ==="
MCUBOOT_ELF="$MCUBOOT_DIR/zephyr.elf"
APP_HEX="$ASSEMBLY_FW_DIR/zephyr.signed.hex"

echo "Flashing MCUBoot bootloader: $MCUBOOT_ELF"
echo "Flashing assembly test app: $APP_HEX"

if [ ! -f "$MCUBOOT_ELF" ]; then
	echo "ERROR: MCUBoot ELF not found at $MCUBOOT_ELF"
	exit 1
fi
if [ ! -f "$APP_HEX" ]; then
	echo "ERROR: Assembly test app hex not found at $APP_HEX"
	exit 1
fi
python3 -m pyocd flash -t "$PYOCD_TARGET" --erase auto "$MCUBOOT_ELF" "$APP_HEX"

# ---- Step 3: DMC reset (simulate power-on) ----
echo "=== Step 3: DMC reset after assembly flash ==="
python3 "$TT_Z_P_ROOT"/scripts/dmc_reset.py

# ---- Step 4: Verify PCIe enumeration (assembly test FW + preflash) ----
echo "=== Step 4: Verify PCIe enumeration (assembly test FW) ==="
verify_pcie_enumeration "assembly test FW"

# ---- Step 5: Flash full production fwbundle to SPI over PCIe (SMC on ARC) ----
echo "=== Step 5: Flash production firmware bundle to SPI (PCIe / SMC) ==="
FWBUNDLE_MATCHES=("$FWBUNDLE_DIR"/fw_pack*.fwbundle)
if [ ! -e "${FWBUNDLE_MATCHES[0]}" ]; then
	echo "ERROR: No firmware bundle found in $FWBUNDLE_DIR"
	exit 1
fi
FWBUNDLE="${FWBUNDLE_MATCHES[0]}"
echo "Using firmware bundle: $FWBUNDLE"
python3 "$TT_Z_P_ROOT"/scripts/smc_spi_flash.py \
	--board-name "$BOARD" -v flash-fwbundle "$FWBUNDLE"

# ---- Step 6: DMC reset (simulate cold boot after bootstrap) ----
echo "=== Step 6: DMC reset after production flash ==="
python3 "$TT_Z_P_ROOT"/scripts/dmc_reset.py

# ---- Step 7: Verify PCIe enumeration (production FW) ----
echo "=== Step 7: Verify PCIe enumeration (production FW) ==="
verify_pcie_enumeration "production FW"

# ---- Step 8: Verify ARC + DMC are responsive (proves production FW booted) ----
echo "=== Step 8: Verify production firmware is running ==="
for ASIC_ID in $(seq 0 $((NUM_ASICS - 1))); do
	echo "Checking ASIC $ASIC_ID..."
	python3 "$TT_Z_P_ROOT"/scripts/check_card.py \
		--asic-id "$ASIC_ID" --timeout 60
done

echo "=== Manufacturing test completed successfully! ==="
