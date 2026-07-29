#!/bin/sh

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

set -e

if [ -z "$BUNDLE_TEMP_PREFIX" ]; then
  TEMP_DIR="$(mktemp -d)/"
  echo "Created TEMP_DIR"
else
  echo "Using existing BUNDLE_TEMP_PREFIX: $BUNDLE_TEMP_PREFIX"
  TEMP_DIR="$BUNDLE_TEMP_PREFIX"
fi

cleanup() {
    if [ -z "$BUNDLE_TEMP_PREFIX" ] && [ -d "$TEMP_DIR" ]; then
        rm -Rf "$TEMP_DIR"
        echo "Removed TEMP_DIR"
    fi
}
trap cleanup EXIT

mPATH="$(dirname "$0")"
TTZP_BASE="$(dirname "$mPATH")"
# put tt_fwbundle.py in the path (use it to combine .fwbundle files)
PATH="$mPATH:$PATH"

BOARD_REVS="$(jq -r -c ".[]" "$TTZP_BASE/.github/boards.json")"

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
  EXTRAVERSION_NUMBER="$(echo "$EXTRAVERSION" | sed 's/[^0-9]*//g')"
else
  EXTRAVERSION_NUMBER=0
fi

RELEASE="$MAJOR.$MINOR.$PATCH$EXTRAVERSION"
PRELEASE="$MAJOR.$MINOR.$PATCH.$EXTRAVERSION_NUMBER"

echo "Building release $RELEASE / pack $PRELEASE"

PIDS=""
FAIL=0

for REV in $BOARD_REVS; do
  BOARD="$($TTZP_BASE/scripts/rev2board.sh "$REV")"

  if [ -n "$BUNDLE_TEMP_PREFIX" ]; then
    if [ -f "${TEMP_DIR}${REV}/update.fwbundle" ]; then
      echo "Using pre-built ${TEMP_DIR}${REV}/update.fwbundle"
      continue
    fi
    echo "Warning: pre-built ${TEMP_DIR}${REV}/update.fwbundle not found"
  fi

  echo "Building $BOARD (rev $REV) -> ${TEMP_DIR}${REV}.log"
  (
    west build -d "${TEMP_DIR}${REV}" --sysbuild -p -b "$BOARD" app/smc \
      >"${TEMP_DIR}${REV}.log" 2>&1
  ) &
  PIDS="$PIDS $!"
done

for pid in $PIDS; do
  wait "$pid" || FAIL=1
done

if [ "$FAIL" -ne 0 ]; then
  echo "one or more builds failed; see ${TEMP_DIR}*.log"
  exit 1
fi

echo "Creating fw_pack-$RELEASE.fwbundle"
# construct arguments..
ARGS="$PWD/$TTZP_BASE/zephyr/blobs/fw_pack-grayskull.tar.gz"
ARGS="$ARGS $PWD/$TTZP_BASE/zephyr/blobs/fw_pack-wormhole.tar.gz"
for REV in $BOARD_REVS; do
  ARGS="$ARGS ${TEMP_DIR}${REV}/update.fwbundle"
done

# shellcheck disable=SC2086
tt_fwbundle.py combine \
  -o "fw_pack-$RELEASE.fwbundle" \
  $ARGS
