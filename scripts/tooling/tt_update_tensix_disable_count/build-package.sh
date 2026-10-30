#!/bin/bash

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

# cd to the script's directory
cd "$(dirname "${BASH_SOURCE[0]}")"
cd tt_update_tensix_disable_count

echo "Linking source files into package directory..."
# Symlink the relevant source files into the package directory
PYTHON_FILES=(
    "fwtable_tooling.py"
    "tt_boot_fs.py"
    "update_tensix_disable_count.py"
)
for file in "${PYTHON_FILES[@]}"; do
    ln -s "../../../$file" "$file"
done

# Symlink data files
for file in "flash_info.proto" "fw_table.proto" "read_only.proto"; do
    ln -s "../../../../drivers/misc/bh_fwtable/spirom_protobufs/$file" "$file"
done

echo "Building the package..."
cd ..
python3 -m build
