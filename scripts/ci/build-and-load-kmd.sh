#!/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tenstorrent AI ULC

# Build a tt-kmd release tag against the running kernel and load tenstorrent.ko.
#
# Intended for privileged CI job containers on self-hosted runners. The module
# is not built in the job container itself: the kernel build tree under
# /usr/src contains host tools (modpost, objtool, fixdep) linked against the
# host's glibc, which fail in a job container with an older userland (e.g. a
# 22.04 image on a 24.04 host). Instead, a short-lived sibling container is
# started through the host's Docker daemon from an image that matches the host
# OS, the module is built there against the host's /lib/modules and /usr/src,
# copied out with `docker cp`, and loaded from the job container.
#
# Requirements: /var/run/docker.sock (GitHub Actions mounts it into job
# containers), CAP_SYS_MODULE in the job container, and kernel headers for the
# running kernel installed on the host.
#
# Environment overrides:
#   KMD_REPO          tt-kmd git URL (default: upstream GitHub)
#   KMD_BUILD_IMAGE   builder image (default: derived from the host OS)

set -euo pipefail

usage() {
	echo "Usage: $(basename "$0") <kmd-version> [--force]" >&2
	echo "Example: $(basename "$0") 2.12.0" >&2
	exit 1
}

if [ $# -lt 1 ] || [ $# -gt 2 ] || [ -z "$1" ]; then
	usage
fi

KMD_VERSION="$1"
FORCE=0
if [ "${2:-}" = "--force" ]; then
	FORCE=1
elif [ -n "${2:-}" ]; then
	usage
fi
KMD_TAG="ttkmd-${KMD_VERSION}"
KVER="$(uname -r)"
KMD_REPO="${KMD_REPO:-https://github.com/tenstorrent/tt-kmd.git}"
DOCKER_CLI_VERSION="27.5.1"

if [ "$(id -u)" -eq 0 ]; then
	SUDO=()
else
	SUDO=(sudo)
fi

echo "Running kernel: ${KVER}"
if [ "$FORCE" -eq 0 ] && [ -d /sys/module/tenstorrent ]; then
	LOADED="$(cat /sys/module/tenstorrent/version)"
	if [ "$LOADED" = "$KMD_VERSION" ]; then
		echo "tt-kmd loaded: ${LOADED} (already present, skip rebuild)"
		exit 0
	fi
fi

if [ ! -e "/lib/modules/${KVER}/build" ]; then
	echo "Cannot build KMD: /lib/modules/${KVER}/build is missing." >&2
	echo "Install linux-headers-${KVER} on the runner host." >&2
	exit 1
fi

WORK_DIR="$(mktemp -d)"
BUILDER=""
cleanup() {
	if [ -n "$BUILDER" ]; then
		"${DOCKER[@]}" rm -f "$BUILDER" >/dev/null 2>&1 || true
	fi
	rm -rf "$WORK_DIR"
}
trap cleanup EXIT

# Job-container tools: kmod for rmmod/insmod, curl to fetch a Docker CLI.
NEED_PKGS=()
command -v insmod >/dev/null 2>&1 || NEED_PKGS+=(kmod)
if ! command -v docker >/dev/null 2>&1 && ! command -v curl >/dev/null 2>&1; then
	NEED_PKGS+=(ca-certificates curl)
fi
if [ ${#NEED_PKGS[@]} -gt 0 ]; then
	"${SUDO[@]}" apt-get update -qq
	"${SUDO[@]}" env DEBIAN_FRONTEND=noninteractive \
		apt-get install -y -qq --no-install-recommends "${NEED_PKGS[@]}"
fi

# The job image may not ship a Docker client; fetch the static one if needed.
if command -v docker >/dev/null 2>&1; then
	DOCKER=(docker)
else
	echo "Fetching Docker CLI ${DOCKER_CLI_VERSION}"
	curl -fsSL \
		"https://download.docker.com/linux/static/stable/x86_64/docker-${DOCKER_CLI_VERSION}.tgz" \
		| tar -xz -C "$WORK_DIR" docker/docker
	DOCKER=("$WORK_DIR/docker/docker")
fi
if [ ! -w /var/run/docker.sock ]; then
	DOCKER=("${SUDO[@]}" "${DOCKER[@]}")
fi

# Pick a builder image whose userland matches the host, so the host-built
# tools in the kernel build tree run.
BUILD_IMAGE="${KMD_BUILD_IMAGE:-}"
if [ -z "$BUILD_IMAGE" ]; then
	HOST_OS="$("${DOCKER[@]}" info --format '{{.OperatingSystem}}')"
	echo "Runner host OS: ${HOST_OS}"
	if [[ "$HOST_OS" =~ ^Ubuntu\ ([0-9]+\.[0-9]+) ]]; then
		BUILD_IMAGE="ubuntu:${BASH_REMATCH[1]}"
	elif [[ "$HOST_OS" =~ ^Debian\ GNU/Linux\ ([0-9]+) ]]; then
		BUILD_IMAGE="debian:${BASH_REMATCH[1]}"
	else
		echo "Cannot pick a KMD builder image for host OS '${HOST_OS}';" \
			"set KMD_BUILD_IMAGE." >&2
		exit 1
	fi
fi
echo "Building ${KMD_TAG} from ${KMD_REPO} in ${BUILD_IMAGE}"

# Runs inside the builder. Ubuntu's kernel headers invoke the exact gcc the
# kernel was built with (e.g. gcc-13), so install that one if the archive has
# it; otherwise any gcc produces a loadable module, since vermagic does not
# record the compiler.
# shellcheck disable=SC2016
BUILD_SCRIPT='
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential ca-certificates git libelf-dev >/dev/null
KCC="$(sed -n "s/^CONFIG_CC_VERSION_TEXT=\"\(.*\)\"$/\1/p" \
	"/lib/modules/${KVER}/build/.config" | grep -oE "gcc-[0-9]+" | head -n1 || true)"
if [ -n "$KCC" ] && ! command -v "$KCC" >/dev/null 2>&1; then
	apt-get install -y -qq --no-install-recommends "$KCC" >/dev/null || true
fi
CC=gcc
if [ -n "$KCC" ] && command -v "$KCC" >/dev/null 2>&1; then
	CC="$KCC"
fi
echo "Compiler: $("$CC" --version | head -n1) (kernel wants ${KCC:-unknown})"
git clone -q --depth 1 --branch "$KMD_TAG" "$KMD_REPO" /tt-kmd
make -C /tt-kmd CC="$CC" KDIR="/lib/modules/${KVER}/build"
'

# /lib/modules and /usr/src are host paths, so they mount the same way into
# the sibling container. Nothing under the job container's own filesystem can
# be mounted (the daemon resolves -v paths on the host), so the module comes
# back through `docker cp`.
BUILDER="kmd-build-$$-$RANDOM"
"${DOCKER[@]}" run --name "$BUILDER" \
	-v /lib/modules:/lib/modules:ro \
	-v /usr/src:/usr/src:ro \
	-e KVER="$KVER" -e KMD_TAG="$KMD_TAG" -e KMD_REPO="$KMD_REPO" \
	"$BUILD_IMAGE" bash -c "$BUILD_SCRIPT"
"${DOCKER[@]}" cp "$BUILDER:/tt-kmd/tenstorrent.ko" "$WORK_DIR/tenstorrent.ko"

# Check /sys/module, not `lsmod | grep -q` (SIGPIPE + pipefail skips rmmod).
if [ -d /sys/module/tenstorrent ]; then
	echo "Unloading tenstorrent $(cat /sys/module/tenstorrent/version)"
	"${SUDO[@]}" rmmod tenstorrent
fi

"${SUDO[@]}" insmod "$WORK_DIR/tenstorrent.ko"

LOADED="$(cat /sys/module/tenstorrent/version)"
echo "tt-kmd loaded: ${LOADED}"
if [ "$LOADED" != "$KMD_VERSION" ]; then
	echo "Loaded KMD version '${LOADED}' does not match requested '${KMD_VERSION}'" >&2
	exit 1
fi
