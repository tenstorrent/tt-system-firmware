#!/usr/bin/env bash

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

USAGE="Usage: $0 <rc|update-rc|patch|patch-rc|pre-release|post-release|post-branch>

  rc           - start new cycle: MINOR++, PATCHLEVEL=0, EXTRAVERSION=rc1
  update-rc    - increment rc number: rc1→rc2, rc2→rc3, etc.
  patch        - increment patch number: PATCHLEVEL++
  patch-rc     - start new patch cycle: PATCHLEVEL++, EXTRAVERSION=rc1
  pre-release  - remove rc tag (prepare stable release)
  post-release - final stable: PATCHLEVEL=99, EXTRAVERSION=
  post-branch  - update main after creating the branch: MINOR++, PATCHLEVEL=99, EXTRAVERSION=
"

[[ $# -eq 1 ]] || { echo "$USAGE" >&2; exit 2; }

case "$1" in
    rc|update-rc|patch|patch-rc|pre-release|post-release|post-branch) MODE="$1" ;;
    -h|--help) echo "$USAGE"; exit 0 ;;
    *) echo "Error: Invalid argument '$1'" >&2; echo "$USAGE" >&2; exit 2 ;;
esac

# Find the base directory of the repo
PROJECT_ROOT=$(cd $(dirname $0); git rev-parse --show-toplevel)
test -f ${PROJECT_ROOT}/scripts/$(basename $0) || { echo "cannot find project root"; exit 1; }

ROOT_VERSION="$PROJECT_ROOT/VERSION"
SMC_VERSION="$PROJECT_ROOT/app/smc/VERSION"
DMC_VERSION="$PROJECT_ROOT/app/dmc/VERSION"

get_version() {
    awk -F '=' '
    /VERSION_MAJOR/   {gsub(/[ "]/,"",$2); major=$2}
    /VERSION_MINOR/   {gsub(/[ "]/,"",$2); minor=$2}
    /PATCHLEVEL/      {gsub(/[ "]/,"",$2); patch=$2}
    /EXTRAVERSION/    {gsub(/[ "]/,"",$2); extra=$2}
    END {
        printf "%s %s %s %s\n", major, minor, patch, (extra ? extra : "")
    }' "$1"
}

update_file() {
    local file="$1"
    local name="$2"

    read -r major minor patch extra < <(get_version "$file")

    local new_minor=$minor
    local new_patch=$patch
    local new_extra=""

    case "$MODE" in
        rc)
            ((new_minor++))
            new_patch=0
            new_extra="rc1"
            ;;
        update-rc)
            # Extract rc number and increment it
            if [[ "$extra" =~ ^rc([0-9]+)$ ]]; then
                local rc_num="${BASH_REMATCH[1]}"
                ((rc_num++))
                new_extra="rc${rc_num}"
            else
                echo "Error: Current version does not have an rc tag (found: '$extra')" >&2
                exit 1
            fi
            ;;
        patch)
            ((new_patch++))
            ;;
        patch-rc)
            ((new_patch++))
            new_extra="rc1"
            ;;
        pre-release)
            new_extra=""
            ;;
        post-release)
            new_patch=99
            new_extra=""
            ;;
        post-branch)
            ((new_minor++))
            new_patch=99
            new_extra=""
            ;;
    esac

    # Only touch lines that actually change
    if [[ $new_minor -ne $minor ]]; then
        sed -i.bak -e "s/^VERSION_MINOR *=.*/VERSION_MINOR = $new_minor/" "$file"
        rm -f "${file}.bak"
    fi

    if [[ $new_patch -ne $patch ]]; then
        sed -i.bak -e "s/^PATCHLEVEL *=.*/PATCHLEVEL = $new_patch/" "$file"
        rm -f "${file}.bak"
    fi

    # EXTRAVERSION line
    sed -i.bak "s/^EXTRAVERSION[[:space:]]*=.*/EXTRAVERSION =${new_extra:+ $new_extra}/" "$file"
    rm -f "${file}.bak"

    local ver="$major.$new_minor.$new_patch"
    [[ -n "$new_extra" ]] && ver+="-$new_extra"
    echo "$ver"
}

do_commit() {
    local file="$1" ver="$2" prefix="${3:-}"

    (cd "$PROJECT_ROOT" && git add "$file")

    case "$MODE" in
        rc)           msg="update to $ver" ;;
        update-rc)    msg="update to $ver" ;;
        patch)        msg="update to $ver" ;;
        patch-rc)     msg="update to $ver" ;;
        pre-release)  msg="update to $ver" ;;
        post-release) msg="update to $ver" ;;
        post-branch)  msg="update to $ver" ;;
    esac

    (cd "$PROJECT_ROOT" && git commit -sm "${prefix}version: $msg

Bump version to $ver")
}

new_smc=$(update_file "$SMC_VERSION" "SMC")
do_commit "$SMC_VERSION" "$new_smc" "app: smc: "

new_dmc=$(update_file "$DMC_VERSION" "DMC")
do_commit "$DMC_VERSION" "$new_dmc" "app: dmc: "

new_ver=$(update_file "$ROOT_VERSION" "root")
do_commit "$ROOT_VERSION" "$new_ver" ""

echo
echo "All done!"
echo "   Root → $new_ver"
echo "    SMC → $new_smc"
echo "    DMC → $new_dmc"
