#!/bin/sh

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

CLUSTER="smc"

if [ $# -eq 0 ]; then
  >&2 echo "Usage: $0 <board> [<cluster>]"
  >&2 echo "Example: $0 p100a -> tt_blackhole@p100a/tt_blackhole/smc"
  >&2 echo "Example: $0 p100a smc -> tt_blackhole@p100a/tt_blackhole/smc"
  >&2 echo "Example: $0 p100a dmc -> tt_blackhole@p100a/tt_blackhole/dmc"
  exit 1
fi

if [ $# -ge 2 ]; then
  CLUSTER="$2"
fi

case "$1" in
  p100a|p150a|p150b|p150c|p300a|p300b|p300c|galaxy|galaxy_revc|orion_slt)
  BOARD="tt_blackhole@$1/tt_blackhole/$CLUSTER";;
  loudbox)
  BOARD="tt_blackhole@p150b/tt_blackhole/$CLUSTER";;
  quietbox2)
  BOARD="tt_blackhole@p300c/tt_blackhole/$CLUSTER";;
  *) >&2 echo "Unknown board: $1"; exit 1;;
esac

echo "$BOARD"
