#!/bin/bash

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

# Pre push hook to run compliance checks on a branch. Saves developers from
# waiting on CI to complete

set -e

# Read values git passes from stdin and discard them
while read local_ref local_oid remote_ref remote_oid; do
	:
done

echo "## Running compliance checks on commits. To skip these checks, run git push --no-verify ##"

# Git worktrees set GIT_DIR to .git/worktrees/<name>. West then treats this
# checkout as a new workspace and fails importing zephyr/west.yml.
unset GIT_DIR GIT_WORK_TREE

if [ -z "${ZEPHYR_BASE}" ]; then
	zep_base=$(west list -f "{abspath}" zephyr)
	echo "ZEPHYR_BASE not set, using $zep_base"
	export ZEPHYR_BASE="${zep_base}"
else
	zep_base="${ZEPHYR_BASE}"
fi

manifest_base=$(west list -f "{abspath}" manifest)

$manifest_base/scripts/check_sysfw_compliance.py \
	-no /dev/null \
	-c main..$HEAD

$manifest_base/scripts/check-copyright.py
