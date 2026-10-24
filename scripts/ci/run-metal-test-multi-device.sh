#!/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tenstorrent AI ULC

# Run a metal test binary on several PCIe devices at the same time, one child
# process per device.
#
# Usage:
#   run-metal-test-multi-device.sh <test_binary> [--separate-cache]
#       [--single-visible-device] [--sequential] [--fast-dispatch]
#       [--hide-build-logs]
#       [--test-filter <gtest_filter>] [--num-devices <n>] [-- <binary args>...]
#
# - With --single-visible-device, each process sees only its own device
#   (TT_VISIBLE_DEVICES=<pcie bdf>).
# - With --separate-cache, each process uses its own TT_METAL_CACHE
#   ($HOME/.cache/tt_metal_<pcie bdf>).
# - With --sequential, the processes run one after another instead of at the
#   same time.
# - The processes run in slow dispatch mode (TT_METAL_SLOW_DISPATCH_MODE=1)
#   unless --fast-dispatch is given.
# - With --hide-build-logs, the JIT build output (compiler commands and
#   diagnostics, e.g. profiler zone "#pragma message" notes) is removed from
#   the printed logs. The log files keep the full output.
# - Without these flags, the processes are launched without TT_VISIBLE_DEVICES
#   and TT_METAL_CACHE.
# - The output of each process goes to $LOG_DIR/dev<n>.log (default: logs).
# - Args after "--" are passed to the binary verbatim instead of --gtest_filter.
#
# The script exits non-zero if any of the processes failed.

usage() {
	echo "usage: $0 <test_binary> [--separate-cache] [--single-visible-device]" >&2
	echo "       [--sequential] [--fast-dispatch] [--hide-build-logs]" >&2
	echo "       [--test-filter <gtest_filter>] [--num-devices <n>] [-- <binary args>...]" >&2
	exit 2
}

BIN=""
SEPARATE_CACHE=0
SINGLE_VISIBLE_DEVICE=0
SEQUENTIAL=0
SLOW_DISPATCH=1
HIDE_BUILD_LOGS=0
FILTER="DramKernel*"
NUM_DEVICES=8
LOG_DIR=${LOG_DIR:-logs}
BIN_ARGS=()

while [ $# -gt 0 ]; do
	case "$1" in
		--separate-cache) SEPARATE_CACHE=1; shift ;;
		--single-visible-device) SINGLE_VISIBLE_DEVICE=1; shift ;;
		--sequential) SEQUENTIAL=1; shift ;;
		--fast-dispatch) SLOW_DISPATCH=0; shift ;;
		--hide-build-logs) HIDE_BUILD_LOGS=1; shift ;;
		--test-filter) FILTER=${2:?--test-filter needs a value}; shift 2 ;;
		--num-devices) NUM_DEVICES=${2:?--num-devices needs a value}; shift 2 ;;
		--) shift; BIN_ARGS=("$@"); break ;;
		-h | --help) usage ;;
		-*) echo "unknown option: $1" >&2; usage ;;
		*) [ -z "$BIN" ] || usage; BIN=$1; shift ;;
	esac
done

[ -n "$BIN" ] || usage
if ! [[ "$NUM_DEVICES" =~ ^[1-9][0-9]*$ ]]; then
	echo "--num-devices must be a positive integer" >&2
	exit 2
fi

[ ${#BIN_ARGS[@]} -gt 0 ] || BIN_ARGS=("--gtest_filter=$FILTER")

# Get the BDF (Bus:Device.Function) of every Tenstorrent PCIe device, in order.
TT_PCI_VID=0x1e52
BDFS=()
for d in /sys/bus/pci/devices/*; do
	[ "$(cat "$d/vendor" 2>/dev/null)" = "$TT_PCI_VID" ] && BDFS+=("$(basename "$d")")
done
BDFS=($(printf '%s\n' "${BDFS[@]}" | sort))
if [ ${#BDFS[@]} -lt "$NUM_DEVICES" ]; then
	echo "requested $NUM_DEVICES devices but found only ${#BDFS[@]}" \
		"Tenstorrent PCIe devices" >&2
	exit 1
fi

mkdir -p "$LOG_DIR"

# Run each child in its own process group, so that cleanup can kill the binary
# and anything it spawned. Kill any child that is still running when this
# script exits.
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
	for pid in "${alive[@]}"; do
		kill -KILL -- "-$pid" 2>/dev/null
	done
	wait 2>/dev/null
}
trap kill_children EXIT
trap 'exit 143' TERM
trap 'exit 130' INT

# Print a log file. With --hide-build-logs, drop each tt-metal JIT build block:
# the "Building <target>, <op> step:" header, the command line after it, and
# the compiler diagnostics that follow.
print_log() {
	if [ "$HIDE_BUILD_LOGS" != 1 ]; then
		cat "$1"
		return
	fi
	awk '
		/^Building [^ ]+, (compile|link) step:$/ { skip = 2; next }
		skip == 2 { skip = 1; next }
		skip && /^(In file included from |[[:space:]]+from |\/[^ ]+:[0-9]+(:[0-9]+)?: |\/[^ ]+: In |[[:space:]]*[0-9]* \|)/ { next }
		{ skip = 0; print }
	' "$1"
}

# Collect the exit code of a child, and print its log in a collapsible group.
rc=0
finish_child() {
	local d=$1
	wait "${pids[$d]}" || { echo "device index $d (${BDFS[$d]}) FAILED (see logs)"; rc=1; }
	echo "::group::Device $d (${BDFS[$d]}) log"
	print_log "$LOG_DIR/dev${d}.log"
	echo "::endgroup::"
}

# Start one child process per device, each with its own log and, if requested,
# its own visible device and cache. With --sequential, wait for each child
# before starting the next.
for d in $(seq 0 $((NUM_DEVICES - 1))); do
	bdf=${BDFS[$d]}
	env_vars=()
	[ "$SLOW_DISPATCH" = 1 ] && env_vars+=(TT_METAL_SLOW_DISPATCH_MODE=1)
	[ "$SINGLE_VISIBLE_DEVICE" = 1 ] && env_vars+=("TT_VISIBLE_DEVICES=$bdf")
	[ "$SEPARATE_CACHE" = 1 ] && env_vars+=("TT_METAL_CACHE=$HOME/.cache/tt_metal_$bdf")
	env "${env_vars[@]}" "$BIN" "${BIN_ARGS[@]}" > "$LOG_DIR/dev${d}.log" 2>&1 &
	pids+=($!)
	[ "$SEQUENTIAL" = 1 ] && finish_child "$d"
done

if [ "$SEQUENTIAL" != 1 ]; then
	for d in $(seq 0 $((NUM_DEVICES - 1))); do
		finish_child "$d"
	done
fi
exit $rc
