#!/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tenstorrent AI ULC

# Check the PCIe link speed/width (and error status) of Tenstorrent devices.
#
# Usage:
#   check-lspci.sh <GEN 1-5> [BDF] [WIDTH=16]
#       Check one device, or every Tenstorrent device if BDF is omitted.
#   check-lspci.sh --board <board>
#       Check every Tenstorrent device against the expectations for <board>.
#
# When checking several devices, every device is checked even if an earlier one
# fails, and the script exits non-zero if any failed. Per-device output is also
# written to $LOG_DIR/pci-width-<bdf>.log (default: logs).
#
# Reading LnkSta and the AER status registers needs CAP_SYS_ADMIN, so in a
# container this needs --cap-add SYS_ADMIN.

LOG_DIR=${LOG_DIR:-logs}
RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[0;33m'
NC='\033[0m' # No Color

# Check the fields that check_link read from lspci. A wrong link speed/width is
# fatal; set bits in the device, AER or lane error status are only warnings.
check_slot() {
	echo "BusID: $BUS_ID"
	echo "$LNK_STA"

	local matches
	matches=$( (grep -E "$SPEED_CHECK" | grep -E "$WIDTH_CHECK") <<< $LNK_STA | wc -l)
	if [ $matches == "0" ]; then
		printf "${RED}ERROR: Bus-ID $BUS_ID Observed: $LNK_STA"
		printf "  Expected: $SPEED_CHECK  $WIDTH_CHECK${NC}\n"
		exit 1
	else
		printf "${GRN}Correct values${NC}\n"
	fi

	echo $DEV_STA
	if [ $(grep "+" <<< $DEV_STA | wc -l) != 0 ]; then
		printf "${YLW}WARNING: Should be all negative(-)${NC}\n"
	else
		printf "${GRN}Correct values${NC}\n"
	fi

	echo $CE_STA
	if [ $(grep "+" <<< $CE_STA | wc -l) != 0 ]; then
		printf "${YLW}WARNING: Should be all negative(-)${NC}\n"
	else
		printf "${GRN}Correct values${NC}\n"
	fi

	echo $UE_STA
	if [ $(grep "+" <<< $UE_STA | wc -l) != 0 ]; then
		printf "${YLW}WARNING: Should be all negative(-)${NC}\n"
	else
		printf "${GRN}Correct values${NC}\n"
	fi

	echo $LANE_ERR_STAT
	if [ $(grep "0" <<< $LANE_ERR_STAT | wc -l) != "1" ]; then
		printf "${YLW}WARNING: Should be 0${NC}\n"
	else
		printf "${GRN}Correct values${NC}\n"
	fi
}

# Check one device. Meant to be run in a subshell, since it exits on failure.
# Args: <GEN 1-5> <BDF> [WIDTH=16]
check_link() {
	local GEN=$1
	local PCIE_BUS_DEV=$2
	local WIDTH=${3:-16} # Default width is 16 if not given

	if [ "$GEN" = 1 ]; then SPEED_CHECK="Speed 2.5GT/s"
	elif [ "$GEN" = 2 ]; then SPEED_CHECK="Speed 5GT/s"
	elif [ "$GEN" = 3 ]; then SPEED_CHECK="Speed 8GT/s"
	elif [ "$GEN" = 4 ]; then SPEED_CHECK="Speed 16GT/s"
	elif [ "$GEN" = 5 ]; then SPEED_CHECK="Speed 32GT/s"
	else
		printf "${RED}PCIE GEN must be 1 to 5${NC}\n"
		exit 1
	fi
	WIDTH_CHECK="Width x$WIDTH"

	if [ "$(lspci -s "$PCIE_BUS_DEV" | wc -l)" -eq 0 ]; then
		echo "No boards reported by lspci for $PCIE_BUS_DEV. Exiting"
		exit 1
	fi
	CMD="sudo lspci -s $PCIE_BUS_DEV -vvv"

	# Run lspci once so every field comes from the same snapshot, and keep stderr
	# so sudo/lspci failures are visible in CI logs.
	LSPCI_OUT=$($CMD 2>&1)
	BUS_ID=$(awk '/b140/ {print $1}' <<< "$LSPCI_OUT")
	LNK_STA=$(grep "LnkSta:" <<< "$LSPCI_OUT" | sed 's/^[ \t]*//;s/[ \t]*$//')
	DEV_STA=$(grep "DevSta:" <<< "$LSPCI_OUT")
	CE_STA=$(grep "CESta:" <<< "$LSPCI_OUT")
	UE_STA=$(grep "UESta:" <<< "$LSPCI_OUT")
	LANE_ERR_STAT=$(grep "LaneErrStat:" <<< "$LSPCI_OUT")

	# No LnkSta usually means lspci could not read the extended config space,
	# so print the uid and effective capabilities to help debug that.
	if [ -z "$LNK_STA" ]; then
		local cap_eff
		cap_eff=$(grep CapEff /proc/self/status | awk '{print $2}')
		printf "${RED}ERROR: no LnkSta in output of '$CMD'"
		printf " (id: $(id -u), caps: $cap_eff). Raw output:${NC}\n"
		echo "$LSPCI_OUT"
		exit 1
	fi

	check_slot
}

# Per-board expectations for one device. Args: <board> <BDF>
check_board_device() {
	local board=$1
	local bdf=$2
	case "$board" in
		bh-galaxy-revc)
			# Devices behind bus 0000:06 are x8, all others x1.
			if readlink -f "/sys/bus/pci/devices/$bdf" | grep -q "/0000:06:"; then
				( check_link 5 "$bdf" 8 )
			else
				( check_link 5 "$bdf" 1 )
			fi
			;;
		quietbox2)
			# Expect x4, retry once expecting x8.
			( check_link 4 "$bdf" 4 ) || ( check_link 4 "$bdf" 8 )
			;;
		loudbox)
			# Default width (x16).
			( check_link 5 "$bdf" )
			;;
	esac
}

# Run "<check_fn> [args...] <BDF>" for every Tenstorrent device, keeping going
# after failures. Returns non-zero if any device failed.
check_all_devices() {
	local check_fn=$1
	shift

	# -D always prints the PCI domain (0000:01:00.0 rather than 01:00.0).
	local bdfs=() line
	while read -r line; do
		bdfs+=("$line")
	done < <(lspci -D -d 1e52: | awk '{print $1}')
	if [ "${#bdfs[@]}" -eq 0 ]; then
		echo "No Tenstorrent devices found by lspci"
		return 1
	fi

	mkdir -p "$LOG_DIR"
	local rc=0 bdf
	for bdf in "${bdfs[@]}"; do
		echo "::group::$bdf"
		"$check_fn" "$@" "$bdf" 2>&1 | tee "$LOG_DIR/pci-width-${bdf//:/_}.log"
		[ "${PIPESTATUS[0]}" -eq 0 ] || { echo "$bdf FAILED"; rc=1; }
		echo "::endgroup::"
	done
	return $rc
}

# Adapter so check_all_devices can pass the BDF last: <GEN> <WIDTH> <BDF>
check_gen_width() {
	( check_link "$1" "$3" "$2" )
}

usage() {
	echo "usage: $0 <GEN 1-5> [BDF] [WIDTH=16]" >&2
	echo "       $0 --board <board>" >&2
	exit 2
}

case "$1" in
	--board)
		BOARD=${2:?--board needs a value}
		case "$BOARD" in
			bh-galaxy-revc | quietbox2 | loudbox) ;;
			*) echo "No PCIe expectations defined for $BOARD, skipping"; exit 0 ;;
		esac
		check_all_devices check_board_device "$BOARD"
		exit $?
		;;
	"" | -*) usage ;;
esac

GEN=$1
BDF=$2
WIDTH=${3:-16}
if [ -n "$BDF" ]; then
	( check_link "$GEN" "$BDF" "$WIDTH" )
	exit $?
fi
check_all_devices check_gen_width "$GEN" "$WIDTH"
exit $?
