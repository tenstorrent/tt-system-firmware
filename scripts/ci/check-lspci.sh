#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tenstorrent AI ULC

# Check PCIe link speed/width (and error status) of a Tenstorrent device.
# Usage: check-lspci.sh <GEN 1-5> [BDF] [WIDTH=16]
RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[0;33m'
NC='\033[0m' # No Color

check_slot() {
    echo "BusID: $BUS_ID"
    echo "$LNK_STA"

    if [ `(grep -E "$SPEED_CHECK" | grep -E "$WIDTH_CHECK") <<< $LNK_STA | wc -l` == "0" ]; then
        printf "${RED}ERROR: Bus-ID $BUS_ID Observed: $LNK_STA  Expected: $SPEED_CHECK  $WIDTH_CHECK${NC}\n";
        exit 1
    else 
        printf "${GRN}Correct values${NC}\n";
    fi

    echo $DEV_STA
    if [ `grep "+" <<< $DEV_STA | wc -l` != 0 ]; then
        printf "${YLW}WARNING: Should be all negative(-)${NC}\n"
    else
        printf "${GRN}Correct values${NC}\n";
    fi

    echo $CE_STA
    if [ `grep "+" <<< $CE_STA | wc -l` != 0 ]; then
        printf "${YLW}WARNING: Should be all negative(-)${NC}\n"
    else 
        printf "${GRN}Correct values${NC}\n";
    fi

    echo $UE_STA
    if [ `grep "+" <<< $UE_STA | wc -l` != 0 ]; then
        printf "${YLW}WARNING: Should be all negative(-)${NC}\n"
    else 
        printf "${GRN}Correct values${NC}\n";
    fi

    echo $LANE_ERR_STAT
    if [ `grep "0" <<< $LANE_ERR_STAT | wc -l` != "1" ]; then
        printf "${YLW}WARNING: Should be 0${NC}\n"
    else 
        printf "${GRN}Correct values${NC}\n";
    fi
}

GEN=$1
PCIE_BUS_DEV=$2
WIDTH=${3:-16}  # Default width is 16 if not given

if   [ "$GEN" = 1 ]; then SPEED_CHECK="Speed 2.5GT/s"
elif [ "$GEN" = 2 ]; then SPEED_CHECK="Speed 5GT/s"
elif [ "$GEN" = 3 ]; then SPEED_CHECK="Speed 8GT/s"
elif [ "$GEN" = 4 ]; then SPEED_CHECK="Speed 16GT/s"
elif [ "$GEN" = 5 ]; then SPEED_CHECK="Speed 32GT/s"
else
    printf "${RED}PCIE GEN must be 1 to 5${NC}\n"; 
    exit 1;
fi
WIDTH_CHECK="Width x$WIDTH"

# The rest (unchanged, but quote variables, fix NUM_BOARDS check):
if [ -n "$PCIE_BUS_DEV" ]; then
    NUM_BOARDS=$(lspci -s "$PCIE_BUS_DEV" | wc -l) # Check grayskull devices only
else
    NUM_BOARDS=$(lspci -d 1e52: | wc -l) # Check grayskull devices only
fi
if [ "$NUM_BOARDS" -eq 0 ]; then echo "No boards reported by lspci. Exiting"; exit 1; fi
if [ -n "$PCIE_BUS_DEV" ]; then
    CMD="sudo lspci -s $PCIE_BUS_DEV -vvv"
else
    CMD="sudo lspci -d :b140 -vvv"
fi
BUS_ID=$($CMD 2>/dev/null | awk '/b140/ {print $1}')
LNK_STA=$($CMD 2>/dev/null | grep "LnkSta:")
LNK_STA=$(echo "$LNK_STA" | sed 's/^[ \t]*//;s/[ \t]*$//')
DEV_STA=$($CMD 2>/dev/null | grep "DevSta:")
CE_STA=$($CMD 2>/dev/null | grep "CESta:")
UE_STA=$($CMD 2>/dev/null | grep "UESta:")
LANE_ERR_STAT=$($CMD 2>/dev/null | grep "LaneErrStat:")

check_slot
