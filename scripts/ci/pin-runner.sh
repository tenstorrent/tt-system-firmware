#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tenstorrent AI ULC

# Add a "runs-on" label list to each CI matrix config, optionally pinning the
# matrix to a single self-hosted runner.
#
# Usage: pin-runner.sh [RUNNER] < configs.json
#
# Reads a JSON array of ci_boards.json entries on stdin and prints it back with
# "runs-on" set on every entry:
# - Without RUNNER, each entry runs on any runner with its board label.
# - With RUNNER, only entries for that runner's board (from ci_runners.json)
#   are kept, and they run on [board, RUNNER]. Requiring the board label too
#   means a stale ci_runners.json entry leaves the job queued instead of
#   flashing the wrong hardware. The runner must have a GitHub label equal to
#   its name.

set -e

TT_Z_P_ROOT=$(realpath "$(dirname "$(realpath "$0")")"/../..)
RUNNERS_JSON="$TT_Z_P_ROOT/.github/ci_runners.json"
RUNNER="$1"

if [ -z "$RUNNER" ]; then
	jq -c 'map(. + {"runs-on": [.board]})'
	exit 0
fi

BOARD=$(jq -r --arg r "$RUNNER" '.[$r].board // empty' "$RUNNERS_JSON")
if [ -z "$BOARD" ]; then
	echo "Unknown runner '$RUNNER', expected one of:" \
		"$(jq -r 'keys | join(", ")' "$RUNNERS_JSON")" >&2
	exit 1
fi

jq -c --arg r "$RUNNER" --arg b "$BOARD" \
	'map(select(.board == $b) | . + {"runs-on": [$b, $r], "runner": $r})'
