#!/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tenstorrent AI ULC

# Build all firmware artifacts required by run-manufacturing-test.sh locally,
# so the manufacturing test can be run without downloading artifacts from a
# previous CI job.
#
# For a given DUT board (e.g. p100a, p150a/b/c, p300a/b/c) this produces:
#   <artifacts>/preflash/preflash-*.ihex         (early SPI preflash)
#   <artifacts>/assembly-mcuboot/zephyr.elf      (assembly DMC MCUBoot)
#   <artifacts>/assembly-fw/zephyr.signed.hex    (assembly DMC test app)
#   <artifacts>/fwbundle/fw_pack-local.fwbundle  (production FW bundle)
#
# It also sets up the prerequisites the pyocd preflash step depends on: the
# STM32G0B1CEUx CMSIS pack and the Blackhole flash loader module (FLM, built
# into scripts/tooling/blackhole_recovery/data/bh_flm/build/).
#
# The preflash rev and assembly-test FW family are derived from the DUT via
# scripts/ci/board-map.sh, matching what CI selects at test time (e.g. a p100a
# DUT uses the p150a preflash and the p150 assembly test firmware).

set -euo pipefail

TT_Z_P_ROOT=$(realpath "$(dirname "$(realpath "$0")")"/..)

ARTIFACTS_DIR="artifacts"
RUN_TEST=0

function print_help {
	echo "Usage: $(basename "$0") [options] <board_name>"
	echo ""
	echo "Build the artifacts required by run-manufacturing-test.sh locally."
	echo ""
	echo "<board_name> is the DUT board revision (e.g. p100a, p150a/b/c, p300a/b/c)."
	echo ""
	echo "Options:"
	echo "  -o <dir>   Output artifacts directory (default: artifacts)"
	echo "  -r         Run run-manufacturing-test.sh after building"
	echo "  -h         Show this help"
}

while getopts "o:rh" opt; do
	case "$opt" in
		o) ARTIFACTS_DIR=$OPTARG ;;
		r) RUN_TEST=1 ;;
		h) print_help; exit 0 ;;
		\?) print_help; exit 1 ;;
	esac
done
shift $((OPTIND - 1))

BOARD="${1:-}"
if [ -z "$BOARD" ]; then
	echo "ERROR: board name is required"
	print_help
	exit 1
fi

# Derive ASSEMBLY_BOARD, NUM_ASICS, PREFLASH_REV, PYOCD_CONFIGS from the DUT.
source "$TT_Z_P_ROOT/scripts/ci/board-map.sh"

# build-preflash.sh only knows how to build p150a / p300a preflash images.
if [[ "$PREFLASH_REV" != "p150a" && "$PREFLASH_REV" != "p300a" ]]; then
	echo "ERROR: unsupported preflash rev '$PREFLASH_REV' for board '$BOARD'"
	exit 1
fi

# Assembly test FW is built for the board *family* (e.g. p150 -> p150a).
ASSEMBLY_REV="${ASSEMBLY_BOARD}a"
ASSEMBLY_DMC_BOARD="$("$TT_Z_P_ROOT/scripts/rev2board.sh" "$ASSEMBLY_REV" dmc)"
SMC_BOARD="$("$TT_Z_P_ROOT/scripts/rev2board.sh" "$BOARD" smc)"

echo "Board:              $BOARD"
echo "Assembly board:     $ASSEMBLY_BOARD (rev $ASSEMBLY_REV -> $ASSEMBLY_DMC_BOARD)"
echo "Preflash rev:       $PREFLASH_REV"
echo "Production SMC board: $SMC_BOARD"
echo "Num ASICs:          $NUM_ASICS"
echo "Artifacts dir:      $ARTIFACTS_DIR"

mkdir -p "$ARTIFACTS_DIR"/{preflash,assembly-mcuboot,assembly-fw,fwbundle}
ARTIFACTS_DIR=$(realpath "$ARTIFACTS_DIR")

BUILD_ROOT="$TT_Z_P_ROOT/build-mfg"
mkdir -p "$BUILD_ROOT"

cd "$TT_Z_P_ROOT"

# ---- 1. pyocd flashing prerequisites (STM32 pack + Blackhole FLM) ----
echo ""
echo "=== [1/5] Installing pyocd flashing prerequisites (STM32 pack) and building FLM ==="
pyocd pack install stm32g0b1ceux
scripts/tooling/blackhole_recovery/data/bh_flm/build-flm.sh

# ---- 2. Preflash ihex ----
echo ""
echo "=== [2/5] Building preflash image ($PREFLASH_REV) ==="
rm -f preflash-*.ihex preflash-*.bin
scripts/build-preflash.sh "$PREFLASH_REV"
cp preflash-*.ihex "$ARTIFACTS_DIR/preflash/"
rm -f preflash-*.ihex preflash-*.bin

# ---- 3 + 4. Assembly test firmware (MCUBoot ELF + signed app hex) ----
echo ""
echo "=== [3/5] Building assembly test firmware ($ASSEMBLY_DMC_BOARD) ==="
ASM_BUILD="$BUILD_ROOT/assembly-$ASSEMBLY_BOARD"
west build -d "$ASM_BUILD" --sysbuild -S rtt-console -p \
	-b "$ASSEMBLY_DMC_BOARD" app/dmc \
	-- -DCONFIG_TT_ASSEMBLY_TEST=y -DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y

echo ""
echo "=== [4/5] Collecting assembly MCUBoot + app artifacts ==="
cp "$ASM_BUILD/mcuboot/zephyr/zephyr.elf"    "$ARTIFACTS_DIR/assembly-mcuboot/"
cp "$ASM_BUILD/dmc/zephyr/zephyr.signed.hex" "$ARTIFACTS_DIR/assembly-fw/"

# ---- 5. Production firmware bundle for the actual DUT ----
echo ""
echo "=== [5/5] Building production firmware bundle ($SMC_BOARD) ==="
SMC_BUILD="$BUILD_ROOT/$BOARD"
west build -d "$SMC_BUILD" --sysbuild -p -b "$SMC_BOARD" app/smc \
	-- -DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y
cp "$SMC_BUILD/update.fwbundle" "$ARTIFACTS_DIR/fwbundle/fw_pack-local.fwbundle"

echo ""
echo "=== Artifacts ready in $ARTIFACTS_DIR ==="
find "$ARTIFACTS_DIR" -type f | sort

if [ "$RUN_TEST" -eq 1 ]; then
	echo ""
	echo "=== Running manufacturing test ==="
	"$TT_Z_P_ROOT/scripts/ci/run-manufacturing-test.sh" \
		-p "$ARTIFACTS_DIR/preflash" \
		-m "$ARTIFACTS_DIR/assembly-mcuboot" \
		-a "$ARTIFACTS_DIR/assembly-fw" \
		-f "$ARTIFACTS_DIR/fwbundle" \
		"$BOARD"
fi
