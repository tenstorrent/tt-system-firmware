#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tenstorrent AI ULC

# This script runs grendel smoke tests

set -e

# Pull required repos
# Clone the VDK utilities repo, which contains a grendel fast functional SIM
git clone git@yyz-gitlab.local.tenstorrent.com:syseng-platform/vdk-utils.git
# Pull tt-smc repo to run Zephyr tests
git clone git@yyz-gitlab.local.tenstorrent.com:tensix/tensix-hw/tt_smc.git

# Locate a single build artifact under the twister output tree. Twister's
# directory layout has changed across versions, so match on the stable tail of
# the path instead of hardcoding it.
find_artifact() {
	local root=$1 pattern=$2
	local matches
	mapfile -t matches < <(find "$root" -path "$pattern" -type f)
	if [ "${#matches[@]}" -eq 0 ]; then
		echo "ERROR: no artifact matching '$pattern' under '$root'" >&2
		exit 1
	fi
	if [ "${#matches[@]}" -gt 1 ]; then
		echo "ERROR: multiple artifacts matching '$pattern' under '$root':" >&2
		printf '  %s\n' "${matches[@]}" >&2
		exit 1
	fi
	printf '%s\n' "${matches[0]}"
}

cd vdk-utils
TWISTER_PLATFORM_DIR=$OUTDIR/tt_mimir_tt_mimir_smc
ZEPHYR_ELF=$(find_artifact "$TWISTER_PLATFORM_DIR" \
	"*/tt-system-firmware/tests/drivers/tt_smc_remoteproc/drivers.tt_smc_remoteproc.bl1_primary/tt_smc_remoteproc/zephyr/zephyr.elf")
PROD_ROM_ELF=../tt_smc/firmware/prod_rom-1.1.1-20260117-794e39bc/build/release/bin/prod_rom.elf
# Watch this command until it outputs "Test PASSED"
mkdir ../vdk-logs
timeout 300 bash -c '
    ./run-smc-headless.sh "$1" "$2" | while read -r line; do
        echo "$line" >> ../vdk-logs/grendel-remoteproc-smoke.log
        if echo "$line" | grep -q "Test PASSED"; then
            echo "Found target output, exiting..."
            exit 0
        fi
    done
' _ "$ZEPHYR_ELF" "$PROD_ROM_ELF"
# Back out of tt-smc repo
cd ..
cd tt_smc
source bin/setup_env.sh
module list || true
bender checkout --force || bender checkout --force
# Patch ttem command to extend test timeout
sed -i "s/run_args: +COCOTB_TEST=smc_zephyr_binary_loader_test"\
" +FW_TEST=grendel_smc_hello_world_smp_zephyr/run_args:"\
" +COCOTB_TEST=smc_zephyr_binary_loader_test"\
" +FW_TEST=grendel_smc_hello_world_smp_zephyr"\
" +FW_TEST_TIMEOUT=1000000000/g" tb_uvm/yaml/regression_smc_chiplet.yaml
UART_BIN=$(find_artifact "$TWISTER_PLATFORM_DIR" \
	"*/tests/drivers/uart/uart_elementary/drivers.uart.uart_elementary.grendel_uart/zephyr/zephyr.bin")
cp "$UART_BIN" \
   ./firmware/zephyr/grendel_smc_hello_world_smp_zephyr/grendel_smc_hello_world_smp_zephyr.bin
ttem tb_uvm/yaml/regression_smc_chiplet.yaml smc_zephyr_hello_world_smp_test \
	--stack flist,cgen,compile_smc_chiplet,sim --no-wave --lsf --seed 1 --c compile_smc_chiplet
