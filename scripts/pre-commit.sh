#!/bin/bash

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

set -e

# Runs simple checkpatch formatting check on each commit

echo "## Running formatting check on commits. To skip this check, run git commit --no-verify ##"

# Git worktrees set GIT_DIR to .git/worktrees/<name>. West then treats this
# checkout as a new workspace and fails importing zephyr/west.yml.
unset GIT_DIR GIT_WORK_TREE

if [ -z "${ZEPHYR_BASE}" ]; then
	zep_base=$(west list -f "{abspath}" zephyr)
	echo "ZEPHYR_BASE not set, using $zep_base"
else
	zep_base="${ZEPHYR_BASE}"
fi

set -e exec
exec git diff --cached | ${zep_base}/scripts/checkpatch.pl -
exec ruff format --diff $(git diff --name-only | grep ".py")
exec ruff check --diff $(git diff --name-only | grep ".py")
