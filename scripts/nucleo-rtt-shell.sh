#!/usr/bin/env bash

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

# Open the rtt:~$ shell of app/dm_test_app on a NUCLEO-U385RG-Q over the
# on-board ST-LINK-V3 with pyocd. Attaches to the running firmware; no reset,
# no flash.
#
#   scripts/nucleo-rtt-shell.sh
#
# The ELF from the last build (see nucleo-dm-test-app.sh) is used only to find
# the RTT control block; without it pyocd scans RAM for it instead.
# pyocd needs the STM32U3 pack once: pyocd pack install stm32u385rg
# Exit the shell with Ctrl-C.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$(dirname "${SCRIPT_DIR}")"

BOARD="nucleo_u385rg_q"
TARGET="${TARGET:-stm32u385rgtxq}"
OUT="${OUT:-${SRC}/build-output/${BOARD}}"
# Set PROBE to an ST-LINK unique ID (see `pyocd list`) when more than one is attached.
PROBE="${PROBE:-}"

if command -v pyocd >/dev/null 2>&1; then
  PYOCD=(pyocd)
else
  PYOCD=(uvx pyocd)
fi
PROBE_ARGS=(-t "${TARGET}")
[[ -n "${PROBE}" ]] && PROBE_ARGS+=(-u "${PROBE}")

# Point pyocd straight at the RTT control block instead of scanning all of RAM.
RTT_ARGS=()
rtt_addr="$(nm "${OUT}/zephyr.elf" 2>/dev/null | awk '$3 == "_SEGGER_RTT" { print $1 }')"
if [[ -n "${rtt_addr}" ]]; then
  RTT_ARGS=(-a "0x${rtt_addr}" -s 0x400)
fi

# attach: leave the core running rather than halting it.
exec "${PYOCD[@]}" rtt "${PROBE_ARGS[@]}" -M attach "${RTT_ARGS[@]}"
