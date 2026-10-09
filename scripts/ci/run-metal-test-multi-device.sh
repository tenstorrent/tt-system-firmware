#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tenstorrent AI ULC

# spawns a child process to run a metal concurrently for each PCIe Device

# usage: ./run-metal-test-multi-device.sh --board <board> <test_binary> [--test-filter <gtest_filter>] [--num-devices <n>] [-- <binary args>...]
# Runs for each PCIe device concurrently 
# On loudbox and bh-galaxy-revc each process sees only its own device (TT_VISIBLE_DEVICES=<pcie bdf>) and uses its own TT_METAL_CACHE 
# On quietbox2 the processes are launched without TT_VISIBLE_DEVICES and TT_METAL_CACHE.
# Logs go to $LOG_DIR (default: logs).
# Args after "--" are passed to the binary verbatim instead of --gtest_filter.

usage() {
  echo "usage: $0 --board <quietbox2|loudbox|bh-galaxy-revc> <test_binary> [--test-filter <gtest_filter>] [--num-devices <n>] [-- <binary args>...]" >&2
  exit 2
}

BOARD=""
BIN=""
FILTER="DramKernel*"
NUM_DEVICES=8
LOG_DIR=${LOG_DIR:-logs}
BIN_ARGS=()

# parse args
while [ $# -gt 0 ]; do
  case "$1" in
    --board) BOARD=${2:?--board needs a value}; shift 2 ;;
    --test-filter) FILTER=${2:?--test-filter needs a value}; shift 2 ;;
    --num-devices) NUM_DEVICES=${2:?--num-devices needs a value}; shift 2 ;;
    --) shift; BIN_ARGS=("$@"); break ;;
    -h|--help) usage ;;
    -*) echo "unknown option: $1" >&2; usage ;;
    *) [ -z "$BIN" ] || usage; BIN=$1; shift ;;
  esac
done

[ -n "$BIN" ] || usage
case "$BOARD" in
  quietbox2|loudbox|bh-galaxy-revc) ;;
  "") echo "--board is required" >&2; usage ;;
  *) echo "unknown board: $BOARD" >&2; usage ;;
esac
[[ "$NUM_DEVICES" =~ ^[1-9][0-9]*$ ]] || { echo "--num-devices must be a positive integer" >&2; exit 2; }

[ ${#BIN_ARGS[@]} -gt 0 ] || BIN_ARGS=("--gtest_filter=$FILTER")

# retrieve list of Tenstorrent PCIe devices by their BDF (Bus:Device.Function)
TT_PCI_VID=0x1e52
BDFS=()
for d in /sys/bus/pci/devices/*; do
  [ "$(cat "$d/vendor" 2>/dev/null)" = "$TT_PCI_VID" ] && BDFS+=("$(basename "$d")")
done
BDFS=($(printf '%s\n' "${BDFS[@]}" | sort))
[ ${#BDFS[@]} -ge "$NUM_DEVICES" ] || {
  echo "requested $NUM_DEVICES devices but found only ${#BDFS[@]} Tenstorrent PCIe devices" >&2
  exit 1
}

mkdir -p "$LOG_DIR"

# Run each child in its own process group so cleanup can kill the binary and anything
# it spawned, and kill any child still running when this script exits
set -m
pids=()
kill_children() {
  local pid alive=()
  for pid in "${pids[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -TERM -- "-$pid" 2>/dev/null
      alive+=("$pid")
    fi
  done
  [ ${#alive[@]} -eq 0 ] && return
  echo "killing leftover child processes: ${alive[*]}" >&2
  sleep 5
  for pid in "${alive[@]}"; do kill -KILL -- "-$pid" 2>/dev/null; done
  wait 2>/dev/null
}
trap kill_children EXIT
trap 'exit 143' TERM
trap 'exit 130' INT

# create a child process with their own log directory, visible device and cache
for d in $(seq 0 $((NUM_DEVICES - 1))); do
  bdf=${BDFS[$d]}
  # for qb2, we do not set TT_VISIBLE_DEVICES or TT_METAL_CACHE
  if [ "$BOARD" = quietbox2 ]; then
    TT_METAL_SLOW_DISPATCH_MODE=1 \
      "$BIN" "${BIN_ARGS[@]}" > "$LOG_DIR/dev${d}.log" 2>&1 &
  else
    TT_METAL_SLOW_DISPATCH_MODE=1 TT_VISIBLE_DEVICES=$bdf \
      TT_METAL_CACHE="$HOME/.cache/tt_metal_$bdf" \
      "$BIN" "${BIN_ARGS[@]}" > "$LOG_DIR/dev${d}.log" 2>&1 &
  fi
  pids+=($!)
done

# collect the child processes' exit codes
rc=0
for d in $(seq 0 $((NUM_DEVICES - 1))); do
  wait "${pids[$d]}" || { echo "device index $d (${BDFS[$d]}) FAILED (see logs)"; rc=1; }
  echo "::group::Device $d (${BDFS[$d]}) log"
  cat "$LOG_DIR/dev${d}.log"
  echo "::endgroup::"
done
exit $rc
