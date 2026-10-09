#!/usr/bin/env bash
# usage: ./run-metal-test-multi-device.sh <test_binary> [--test-filter <gtest_filter>] [--num-devices <n>] [-- <binary args>...]
# Runs the binary once per device (devices 0..n-1) in parallel, each seeing only
# its own device as device 0 (via TT_VISIBLE_DEVICES). Logs go to $LOG_DIR (default: logs).
# Args after "--" are passed to the binary verbatim instead of --gtest_filter.
usage() {
  echo "usage: $0 <test_binary> [--test-filter <gtest_filter>] [--num-devices <n>] [-- <binary args>...]" >&2
  exit 2
}

BIN=""
FILTER="DramKernel*"
NUM_DEVICES=8
LOG_DIR=${LOG_DIR:-logs}
BIN_ARGS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --test-filter) FILTER=${2:?--test-filter needs a value}; shift 2 ;;
    --num-devices) NUM_DEVICES=${2:?--num-devices needs a value}; shift 2 ;;
    --) shift; BIN_ARGS=("$@"); break ;;
    -h|--help) usage ;;
    -*) echo "unknown option: $1" >&2; usage ;;
    *) [ -z "$BIN" ] || usage; BIN=$1; shift ;;
  esac
done

[ -n "$BIN" ] || usage
[[ "$NUM_DEVICES" =~ ^[1-9][0-9]*$ ]] || { echo "--num-devices must be a positive integer" >&2; exit 2; }

[ ${#BIN_ARGS[@]} -gt 0 ] || BIN_ARGS=("--gtest_filter=$FILTER")

mkdir -p "$LOG_DIR"
pids=()
for d in $(seq 0 $((NUM_DEVICES - 1))); do
  TT_METAL_SLOW_DISPATCH_MODE=1 TT_VISIBLE_DEVICES=$d \
    "$BIN" "${BIN_ARGS[@]}" > "$LOG_DIR/dev${d}.log" 2>&1 &
  pids+=($!)
done

rc=0
for i in "${!pids[@]}"; do
  wait "${pids[$i]}" || { echo "device index $i FAILED (see logs)"; rc=1; }
done

# Print each device's log in numerical order so it shows up in the step output.
for d in $(seq 0 $((NUM_DEVICES - 1))); do
  echo "::group::Device $d log"
  cat "$LOG_DIR/dev${d}.log"
  echo "::endgroup::"
done
exit $rc
