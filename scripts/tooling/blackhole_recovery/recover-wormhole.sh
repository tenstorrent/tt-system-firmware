#!/bin/bash

# Copyright (c) 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

IMAGE_URL="ghcr.io/tenstorrent/tt-system-firmware/wh-recovery-image"
IMAGE_TAG=${IMAGE_TAG:-"latest"}
if [[ -n $BOARD_SERIAL ]]; then
    SERIAL_ARG="--board-id ${BOARD_SERIAL}"
fi
if [[ -n $BOARD_TYPE ]]; then
    TYPE_ARG="${BOARD_TYPE}"
fi
if [[ $GITHUB_RUN_ATTEMPT -gt 1 ]]; then
    # Set within github runner environment for retries, if not first attempt
    # let's force recovery
    FORCE_ARG="--force"
fi

# This script will download and run the wormhole recovery tool inside a
# Docker container. The firmware bundle is baked into the image.
if ! command -v docker >/dev/null 2>&1; then
    echo "Docker not installed. Please install Docker to proceed."
    exit 1
fi

if [[ -z $BOARD_TYPE && $# -lt 1 ]]; then
    echo "Usage: $0 <board type>"
    echo "  board type: n300 or n150"
    exit 1
fi

if ! docker info >/dev/null 2>&1; then
    echo "You do not have permission to run Docker commands."\
        "Please ensure your user is in the 'docker' group or run as root."
    exit 1
fi

# Pull the latest image
docker pull $IMAGE_URL:$IMAGE_TAG

echo "Launching docker container to recover wormhole device..."
# Bind mount /dev so the container tracks hotplug events. Docker otherwise gives
# the container a private /dev populated at startup, so a card that enumerates
# during recovery never appears at /dev/tenstorrent/<n> inside the container.
docker run --device /dev/bus/usb --privileged -v /dev:/dev \
    --rm $IMAGE_URL:$IMAGE_TAG \
    python3 /tt-system-firmware/scripts/tooling/blackhole_recovery/recover-wormhole.py \
    /firmware.fwbundle $TYPE_ARG $SERIAL_ARG $FORCE_ARG $@
