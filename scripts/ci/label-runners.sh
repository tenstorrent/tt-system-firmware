#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tenstorrent AI ULC

# Give self-hosted runners a GitHub label equal to their name, so that CI jobs
# can be pinned to them (see pin-runner.sh and the 'runner' input / label of
# hardware-long.yml).
#
# Usage: label-runners.sh [-n] RUNNER...
#        label-runners.sh [-n] --all
#
#   -n     Dry run: show each runner's labels without changing anything
#   --all  Every runner in .github/ci_runners.json
#
# Runners that already have the label are left alone, so this is safe to
# re-run, e.g. after a runner is re-registered (which drops its labels).
#
# Requires the GitHub CLI, logged in as an org owner or runner manager, with
# the admin:org scope (gh auth refresh -s admin:org).

set -e

ORG=tenstorrent
TT_Z_P_ROOT=$(realpath "$(dirname "$(realpath "$0")")"/../..)
RUNNERS_JSON="$TT_Z_P_ROOT/.github/ci_runners.json"

usage() {
	cat >&2 <<-EOF
		Usage: $0 [-n] RUNNER...
		       $0 [-n] --all
	EOF
	exit 1
}

DRY_RUN=0
RUNNERS=()
for arg in "$@"; do
	case "$arg" in
	-n) DRY_RUN=1 ;;
	--all)
		while read -r name; do
			RUNNERS+=("$name")
		done < <(jq -r 'keys[]' "$RUNNERS_JSON")
		;;
	-*) usage ;;
	*) RUNNERS+=("$arg") ;;
	esac
done
[ ${#RUNNERS[@]} -gt 0 ] || usage

label_runner() {
	local name="$1" runner id status labels board

	runner=$(gh api "orgs/$ORG/actions/runners?name=$name" \
		--jq ".runners[] | select(.name == \"$name\")")
	if [ -z "$runner" ]; then
		echo "$name: no such runner in $ORG" >&2
		return 1
	fi
	id=$(jq -r '.id' <<< "$runner")
	status=$(jq -r '.status' <<< "$runner")
	labels=$(jq -r '[.labels[].name] | join(",")' <<< "$runner")
	echo "$name (id $id, $status): $labels"

	# pin-runner.sh runs pinned jobs on [board, name], so a missing board
	# label would leave them queued.
	board=$(jq -r --arg r "$name" '.[$r].board // empty' "$RUNNERS_JSON")
	if [ -z "$board" ]; then
		echo "  warning: not in ci_runners.json, so CI can't be pinned to it" >&2
	elif [[ ",$labels," != *",$board,"* ]]; then
		echo "  warning: missing board label '$board' from ci_runners.json" >&2
	fi

	if [[ ",$labels," == *",$name,"* ]]; then
		echo "  already labelled"
	elif [ $DRY_RUN -eq 1 ]; then
		echo "  would add label '$name'"
	else
		# POST adds to the existing labels, unlike PUT which replaces them
		labels=$(gh api -X POST "orgs/$ORG/actions/runners/$id/labels" \
			-f "labels[]=$name" --jq '[.labels[].name] | join(",")')
		echo "  added, now: $labels"
	fi
}

FAILED=0
for name in "${RUNNERS[@]}"; do
	label_runner "$name" || FAILED=1
done
exit $FAILED
