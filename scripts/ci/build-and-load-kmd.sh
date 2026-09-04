#!/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tenstorrent AI ULC

# Build a tt-kmd release tag against the running kernel and load tenstorrent.ko.
# Intended for privileged CI containers. Bind-mount the host's /lib/modules and
# /usr/src so `make` can use the running kernel's build tree.

set -euo pipefail

if [ $# -ne 1 ] || [ -z "$1" ]; then
	echo "Usage: $(basename "$0") <kmd-version>" >&2
	echo "Example: $(basename "$0") 2.10.0" >&2
	exit 1
fi

KMD_VERSION="$1"
KMD_TAG="ttkmd-${KMD_VERSION}"
KVER="$(uname -r)"
KMD_REPO="${KMD_REPO:-https://github.com/tenstorrent/tt-kmd.git}"

if [ "$(id -u)" -eq 0 ]; then
	SUDO=()
else
	SUDO=(sudo)
fi

export DEBIAN_FRONTEND=noninteractive

echo "Running kernel: ${KVER}"
echo "Building ${KMD_TAG} from ${KMD_REPO}"

"${SUDO[@]}" apt-get update
"${SUDO[@]}" apt-get install -y --no-install-recommends \
	build-essential \
	ca-certificates \
	git \
	kmod

if [ ! -e "/lib/modules/${KVER}/build" ]; then
	echo "Kernel build tree /lib/modules/${KVER}/build not found;" \
		"installing linux-headers-${KVER}"
	"${SUDO[@]}" apt-get install -y --no-install-recommends \
		"linux-headers-${KVER}"
fi

if [ ! -e "/lib/modules/${KVER}/build" ]; then
	echo "Cannot build KMD: /lib/modules/${KVER}/build is missing." >&2
	echo "Bind-mount the host /lib/modules and /usr/src into this" \
		"container, or install linux-headers-${KVER} on the runner." >&2
	exit 1
fi

# Ubuntu's kernel headers invoke the exact gcc the kernel was built with, so
# without it the module build dies with "gcc-N: not found". That version is not
# always in the container's archive: jammy stops at gcc-12, but a host kernel
# built with gcc-13 asks for gcc-13, which only the toolchain PPA carries. A
# module's vermagic does not record the compiler, so if the exact version cannot
# be had, building with another gcc still yields a loadable module.
KERNEL_CC="${KMD_CC:-}"
if [ -z "$KERNEL_CC" ]; then
	CC_VERSION_TEXT="$(sed -n 's/^CONFIG_CC_VERSION_TEXT="\(.*\)"$/\1/p' \
		"/lib/modules/${KVER}/build/.config" 2>/dev/null || true)"
	KERNEL_CC="$(printf '%s\n%s\n' "$CC_VERSION_TEXT" "$(cat /proc/version)" \
		| grep -oE 'gcc-[0-9]+' | head -n1 || true)"
fi

# Set empty to skip the PPA fallback.
TOOLCHAIN_PPA="${KMD_TOOLCHAIN_PPA-ppa:ubuntu-toolchain-r/test}"

have_cc() {
	command -v "$1" >/dev/null 2>&1
}

newest_installed_gcc() {
	printf '%s\n' /usr/bin/gcc-[0-9]* \
		| grep -oE 'gcc-[0-9]+$' \
		| sort -t- -k2,2n \
		| tail -n1
}

if [ -n "$KERNEL_CC" ] && ! have_cc "$KERNEL_CC"; then
	echo "Kernel was built with ${KERNEL_CC}; installing it to match"
	"${SUDO[@]}" apt-get install -y --no-install-recommends "$KERNEL_CC" || true
fi

if [ -n "$KERNEL_CC" ] && ! have_cc "$KERNEL_CC" && [ -n "$TOOLCHAIN_PPA" ]; then
	echo "${KERNEL_CC} is not in this image's archive; trying ${TOOLCHAIN_PPA}"
	if "${SUDO[@]}" apt-get install -y --no-install-recommends \
		software-properties-common; then
		"${SUDO[@]}" add-apt-repository -y "$TOOLCHAIN_PPA" || true
		"${SUDO[@]}" apt-get install -y --no-install-recommends "$KERNEL_CC" || true
	fi
fi

if [ -n "$KERNEL_CC" ] && ! have_cc "$KERNEL_CC"; then
	FALLBACK_CC="$(newest_installed_gcc || true)"
	if [ -z "$FALLBACK_CC" ] && have_cc gcc; then
		FALLBACK_CC="gcc"
	fi
	if [ -z "$FALLBACK_CC" ]; then
		echo "Cannot build KMD: ${KERNEL_CC} is unavailable and no other gcc" \
			"is installed." >&2
		echo "Set KMD_CC to a compiler that is present in this image." >&2
		exit 1
	fi
	echo "${KERNEL_CC} is unavailable; falling back to ${FALLBACK_CC}"
	KERNEL_CC="$FALLBACK_CC"
fi

BUILD_DIR="$(mktemp -d)"
cleanup() {
	rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

git clone --depth 1 --branch "$KMD_TAG" "$KMD_REPO" "$BUILD_DIR/tt-kmd"

MAKE_ARGS=()
if [ -n "$KERNEL_CC" ]; then
	echo "Building with CC=${KERNEL_CC} ($("$KERNEL_CC" --version | head -n1))"
	MAKE_ARGS+=("CC=$KERNEL_CC")
fi

# The host kernel build tree is bind-mounted into the CI container. 24.04
# kernel ships a prebuilt objtool linked against GLIBC 2.38+, which
# cannot run in a 22.04 container (GLIBC 2.35). Skip objtool in that
# case; it is only used for stack validation and is not required to produce a
# loadable module.
#
# OBJECT_FILES_NON_STANDARD=y only skips per-TU objtool. With IBT/LTO, kbuild
# still runs objtool on the linked module (tenstorrent.o), which is where the
# glibc mismatch fails. SKIP_STACK_VALIDATION was removed from kbuild. Point
# kbuild's `objtool` (scripts/Makefile.lib) at a no-op stub instead;
# /usr/src is bind-mounted read-only.
OBJTOOL_BIN="/lib/modules/${KVER}/build/tools/objtool/objtool"
objtool_can_run() {
	local err
	[ -x "$1" ] || return 1
	# Loader failures (missing libs / GLIBC_* not found) go to stderr.
	# A working objtool prints usage on stdout and leaves stderr empty.
	err="$(LC_ALL=C "$1" 2>&1 >/dev/null || true)"
	case "$err" in
	*"not found"*|*"cannot open shared object"*) return 1 ;;
	esac
	return 0
}
if ! objtool_can_run "$OBJTOOL_BIN"; then
	echo "Kernel objtool cannot run in this environment (likely a glibc" \
		"mismatch with bind-mounted host headers); skipping stack validation"
	OBJTOOL_STUB="$BUILD_DIR/objtool-stub"
	printf '#!/bin/sh\nexit 0\n' > "$OBJTOOL_STUB"
	chmod +x "$OBJTOOL_STUB"
	MAKE_ARGS+=("objtool=$OBJTOOL_STUB")
fi

make -C "$BUILD_DIR/tt-kmd" "${MAKE_ARGS[@]+"${MAKE_ARGS[@]}"}"

# Check /sys/module, not `lsmod | grep -q` (SIGPIPE + pipefail skips rmmod).
if [ -d /sys/module/tenstorrent ]; then
	echo "Unloading tenstorrent $(cat /sys/module/tenstorrent/version)"
	"${SUDO[@]}" rmmod tenstorrent
fi

"${SUDO[@]}" insmod "$BUILD_DIR/tt-kmd/tenstorrent.ko"

LOADED="$(cat /sys/module/tenstorrent/version)"
echo "tt-kmd loaded: ${LOADED}"
if [ "$LOADED" != "$KMD_VERSION" ]; then
	echo "Loaded KMD version '${LOADED}' does not match requested '${KMD_VERSION}'" >&2
	exit 1
fi
