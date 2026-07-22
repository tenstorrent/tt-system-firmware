#!/bin/bash

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

# Helper script to build preflash bootfs image for assembly
set -e

# handle temp directory for the build
if [ -z "$BUNDLE_TEMP_PREFIX" ]; then
  TEMP_DIR="$(mktemp -d)/"

  # Sanity check
  if [ "$TEMP_DIR" = "/" ]; then
    echo "ERROR: Failed to create temp dir or invalid TEMP_DIR ('$TEMP_DIR')"
    exit 1
  fi

  echo "Created TEMP_DIR"
else
  echo "Using existing BUNDLE_TEMP_PREFIX: $BUNDLE_TEMP_PREFIX"
  TEMP_DIR="$BUNDLE_TEMP_PREFIX"
fi

cleanup() {
    if [ -z "$BUNDLE_TEMP_PREFIX" ] && [ -d "$TEMP_DIR" ]; then
        rm -rf "$TEMP_DIR"
        echo "Removed TEMP_DIR"
    fi
}
trap cleanup EXIT

# Get the script directory and base paths
SCRIPT_DIR="$(dirname "$0")"
TTZP_BASE="$(dirname "$SCRIPT_DIR")"

# board rev to build pre-flash for (p300a or p150a)
BOARD_REV="${1:-p300a}"
if [[ "$BOARD_REV" != "p300a" && "$BOARD_REV" != "p150a" ]]; then
  echo "ERROR: unsupported board rev '$BOARD_REV' (expected p300a or p150a)"
  echo "Usage: $0 [p300a|p150a]"
  exit 1
fi
BUILD_DIR="$TEMP_DIR"

# firmware version info
MAJOR="$(grep "^VERSION_MAJOR" "$TTZP_BASE/VERSION" | awk '{print $3}')"
MINOR="$(grep "^VERSION_MINOR" "$TTZP_BASE/VERSION" | awk '{print $3}')"
PATCH="$(grep "^PATCHLEVEL" "$TTZP_BASE/VERSION" | awk '{print $3}')"
EXTRAVERSION="$(grep "^EXTRAVERSION" "$TTZP_BASE/VERSION" | awk '{print $3}')"

if [ -z "$MAJOR" ] || [ -z "$MINOR" ] || [ -z "$PATCH" ]; then
  echo "required version info missing: MAJOR=$MAJOR MINOR=$MINOR PATCH=$PATCH"
  exit 1
fi

if [ -n "$EXTRAVERSION" ]; then
  EXTRAVERSION="-$EXTRAVERSION"
fi

RELEASE="$MAJOR.$MINOR.$PATCH$EXTRAVERSION"

echo "Running sysbuild for $BOARD_REV..."
BOARD="$("$SCRIPT_DIR/rev2board.sh" "$BOARD_REV")"
west build -d "$BUILD_DIR" --sysbuild -p -b "$BOARD" "$TTZP_BASE/app/smc" >/dev/null 2>&1
echo "Build completed successfully"

# Create preflash.ihex using preflash-bootfs.yaml
echo "Generating preflash.ihex..."
YAML_PATH="$TTZP_BASE/boards/tenstorrent/tt_blackhole/bootfs/preflash-bootfs.yaml"
$SCRIPT_DIR/tt_boot_fs.py mkfs "$YAML_PATH" "preflash-$RELEASE.ihex" --build-dir "$BUILD_DIR" --hex
objcopy -I ihex -O binary "preflash-$RELEASE.ihex" "preflash-$RELEASE.bin"
echo "preflash.ihex-$RELEASE generated successfully"
