#!/usr/bin/env bash

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

# Build app/dm_test_app for the MK DMC in the firmware CI image.
# Builds the checkout this script lives in; no fetch of a remote branch.
set -euo pipefail

IMAGE="${IMAGE:-ghcr.io/tenstorrent/tt-system-firmware/ci-image:v19.11.0}"
BOARD="${BOARD:-tt_grendel_mk_bu_dmc}"
OUT="${OUT:-${HOME}/fw-out/dm_test_app}"
# Optional devicetree overlay, as a path relative to app/dm_test_app, e.g.
# OVERLAY=mk_i3c_controller.overlay. Applied with EXTRA_DTC_OVERLAY_FILE; the
# merged build/zephyr/zephyr.dts is copied out so the effect can be checked.
OVERLAY="${OVERLAY:-}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$(dirname "${SCRIPT_DIR}")"

mkdir -p "${OUT}"
DOCKER=(docker)
docker info >/dev/null 2>&1 || DOCKER=(sudo docker)

"${DOCKER[@]}" run --rm --entrypoint bash \
  -v "${SRC}:/tt-zephyr/tt-system-firmware" \
  -v "${OUT}:/out" \
  "${IMAGE}" \
  -lc "
set -euo pipefail
git config --global --add safe.directory '*'
cd /tt-zephyr
west packages pip --install
west update
EXTRA=()
if [ -n '${OVERLAY}' ]; then
  EXTRA=(-- -DEXTRA_DTC_OVERLAY_FILE=/tt-zephyr/tt-system-firmware/app/dm_test_app/${OVERLAY})
fi
west build -p -b ${BOARD} tt-system-firmware/app/dm_test_app \${EXTRA[@]+\"\${EXTRA[@]}\"}
cp build/zephyr/zephyr.{bin,hex,elf,dts} /out/
chown -R $(id -u):$(id -g) /out
"

echo "Built ${OUT}/zephyr.hex"
ls -l "${OUT}/zephyr".*
