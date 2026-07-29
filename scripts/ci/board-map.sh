# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tenstorrent AI ULC

# Derive Blackhole board-family properties from a runner board name.
#
# Source this file with $BOARD set (e.g. p100a, p150a, p300a) to populate:
#   ASSEMBLY_BOARD  - board family used to select assembly test FW artifacts
#                      (p100 and p150 share the same assembly FW)
#   NUM_ASICS       - number of ASICs on the board (2 for p300 variants, else 1)
#   PREFLASH_REV    - preflash image revision to use (p150a or p300a)
#   PYOCD_CONFIGS   - bash array with one pyocd FLM user-script per ASIC (in ASIC
#                      order), taken from board_metadata.yaml. Used to program the
#                      external SPI EEPROM over JTAG/SWD with `pyocd`.

# Strip the trailing revision letter, e.g. p150a -> p150.
ASSEMBLY_BOARD=$(echo "$BOARD" | sed 's/[a-z]$//')

# p100 uses the same assembly test firmware as p150.
if [[ "$ASSEMBLY_BOARD" == "p100" ]]; then
	ASSEMBLY_BOARD="p150"
fi

# p300 variants have 2 ASICs, all others have 1.
if [[ "$ASSEMBLY_BOARD" == "p300" ]]; then
	NUM_ASICS=2
else
	NUM_ASICS=1
fi

# p100 and p150 variants use the p150a preflash, all others use p300a.
case "$ASSEMBLY_BOARD" in
	p100 | p150) PREFLASH_REV="p150a" ;;
	*) PREFLASH_REV="p300a" ;;
esac

# Per-ASIC pyocd FLM user-script names, read from board_metadata.yaml. bash has
# no YAML parser, so shell out to python3 (available in the CI container). The
# order matches the ASIC order in the metadata, which is the order the rest of
# the manufacturing test iterates over.
_BOARD_MAP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mapfile -t PYOCD_CONFIGS < <(
	python3 -c '
import sys, yaml
board, meta_path = sys.argv[1], sys.argv[2]
with open(meta_path) as f:
    meta = yaml.safe_load(f)
if board not in meta:
    sys.exit(f"Unknown board {board!r} in {meta_path}")
for asic in meta[board]:
    print(asic["pyocd-config"])
' "$BOARD" "$_BOARD_MAP_DIR/../board_metadata.yaml"
)
