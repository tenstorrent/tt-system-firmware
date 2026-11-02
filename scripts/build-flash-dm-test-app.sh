#!/usr/bin/env bash

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

# Build app/dm_test_app via build-dm-test-app.sh and flash it via pyocd.
#
#   scripts/build-flash-dm-test-app.sh    # builds, then flashes. Always both.
#   FREQ=200k scripts/build-flash-dm-test-app.sh   # ...with a slower SWD clock
#   BOARD=<board> TARGET=<target> PROBE=<probe> OUT=<out> scripts/build-flash-dm-test-app.sh
#
# Everything is an environment variable. The defaults are the GLX2 CMB
# bring-up path, so that one needs no variables at all:
#
#   BOARD   board to build        default tt_blackhole_glx2_dmc
#   TARGET  pyocd target (die)    default stm32u375veix
#   OUT     where the build goes  default <repo>/build-output/$BOARD
#   PROBE   pyocd probe UID (-u)  default empty
#   FREQ    SWD clock, pyocd -f   default empty (pyocd uses 1 MHz)

#   BOARD                  TARGET
#   tt_blackhole_glx2_dmc  stm32u375veix
#   tt_grendel_mk_bu_dmc   stm32u375rgt6
#   nucleo_u385rg_q        stm32u385rgtxq
#
# PROBE is only needed when pyocd can see more than one debug probe; with one
# attached it selects it automatically. `pyocd list` prints the UIDs.
#
# FREQ takes 200k, 1M, or plain hertz. The probe supports only a discrete set
# of clocks and snaps the request down to the nearest one; `pyocd -vv` logs
# which. Roughly 100k to 4M is the useful band on an STLINK-V3. Slowing the
# clock is the first thing to try against a marginal SWD harness -- though a
# link that corrupts transfers at every rate is bad in a way clock speed
# cannot reach, and the harness itself is then the thing to fix.
#
# pyocd needs the CMSIS pack for the die once, e.g.
#   pyocd pack install stm32u375veix

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE_DIR="$(dirname "${SCRIPT_DIR}")"

BOARD="${BOARD:-tt_blackhole_glx2_dmc}"
TARGET="${TARGET:-stm32u375veix}"
OUT="${OUT:-${WORKSPACE_DIR}/build-output/${BOARD}}"
PROBE="${PROBE:-}"
FREQ="${FREQ:-}"
export BOARD TARGET OUT PROBE FREQ

if command -v pyocd >/dev/null 2>&1; then
  PYOCD=(pyocd)
elif command -v uvx >/dev/null 2>&1; then
  # --with pylink-square unconditionally: it is what lets pyocd drive a J-Link,
  # and a plugin cannot be installed once into a throwaway uvx environment.
  PYOCD=(uvx --with pylink-square pyocd)
else
  echo "$0: neither pyocd nor uvx found." >&2
  echo "  install uv:  curl -LsSf https://astral.sh/uv/install.sh | sh" >&2
  exit 1
fi

if ! docker info >/dev/null 2>&1 && ! sudo -n docker info >/dev/null 2>&1; then
  echo "$0: cannot talk to docker, which is where the build runs." >&2
  echo "  check the daemon is up, and that you are in the docker group" >&2
  echo "  (newgrp docker, or log out and back in, after being added)" >&2
  exit 1
fi

# Install the CMSIS pack for this die if it is not already in the per-user
# cache. 'pyocd list --targets' knows a target only once its pack is present.
if ! "${PYOCD[@]}" list --targets 2>/dev/null | grep -qiw -- "${TARGET}"; then
  echo "CMSIS pack for ${TARGET} not found, installing it (first run only)..."
  "${PYOCD[@]}" pack install "${TARGET}"
fi

PROBE_ARGS=(-t "${TARGET}")
if [[ -n "${PROBE}" ]]; then
  PROBE_ARGS+=(-u "${PROBE}")
else
  # No PROBE given, so pyocd will auto-select -- which only works when exactly
  # one probe is attached. Say so now, with the UIDs, rather than let pyocd
  # fail further in or silently pick the wrong board.
  probe_count="$("${PYOCD[@]}" list 2>/dev/null | grep -cE '^ *[0-9]+ ' || true)"
  if [[ "${probe_count}" == "0" ]]; then
    echo "$0: pyocd sees no debug probe." >&2
    echo "  check it is plugged in, and that the udev rules are installed" >&2
    echo "  (boardy: scripts/udev/, applied by ansible/playbooks/setup_lab_host.yml)" >&2
    exit 1
  fi
  if [[ "${probe_count}" != "1" ]]; then
    echo "$0: pyocd sees ${probe_count} probes; set PROBE=<uid> to choose one." >&2
    "${PYOCD[@]}" list >&2
    exit 1
  fi
fi

if [[ -n "${FREQ}" ]]; then
  PROBE_ARGS+=(-f "${FREQ}")
fi

echo "board=${BOARD} target=${TARGET} out=${OUT} probe=${PROBE:-<auto>} freq=${FREQ:-<default>}"

"${SCRIPT_DIR}/build-dm-test-app.sh"

# Checked here rather than left to pyocd, whose error does not name the path it
# wanted: a docker build that exits clean without copying anything out lands
# here with no hex.
if [[ ! -f "${OUT}/zephyr.hex" ]]; then
  echo "$0: build produced no ${OUT}/zephyr.hex" >&2
  exit 1
fi

"${PYOCD[@]}" flash "${PROBE_ARGS[@]}" "${OUT}/zephyr.hex"
