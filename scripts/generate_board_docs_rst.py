#!/usr/bin/env python3
# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0
"""
RST Documentation Generator for Board Configurations

This script automatically generates reStructuredText documentation for board configurations
based on the spirom data tables in the boards directory.
"""

from pathlib import Path
from typing import Dict, Any
from collections import defaultdict
from dataclasses import dataclass, field

# Constants
PB_FILES = ["read_only.txt", "fw_table.txt"]
SECTION_TITLES = {
    "read_only": "Read-Only Configuration",
    "fw_table": "Firmware Table Configuration",
}
HEX_FIELDS = {"board_id", "vendor_id"}
FILE_TYPES = ["read_only", "fw_table"]

# Sections to ignore when generating documentation
IGNORED_SECTIONS = {
    "fan_table",
    "dram_table",
    "chip_harvesting_table",
    "funtest_table",
    "reference_values_table",
}

RST_HEADER = """.. _board_configuration_reference:

===========================================
TT BlackHole Board Configuration Reference
===========================================

"""


@dataclass
class BoardConfig:
    """Structured representation of a board configuration"""

    name: str
    configs: Dict[str, Dict[str, Any]] = field(default_factory=dict)

    def get_config(self, file_type: str) -> Dict[str, Any]:
        """Get configuration for a specific file type"""
        return self.configs.get(file_type, {})


def convert_value(value_str: str) -> Any:
    """Convert string value to appropriate type"""
    value = value_str.strip()

    if value.lower() in ("true", "false"):
        return value.lower() == "true"

    if value.startswith("0x"):
        return int(value, 16)

    if value.lstrip("-").isdigit():
        return int(value)

    return value


def parse_config_file(file_path: Path) -> Dict[str, Any]:
    """Parse a single configuration file"""
    config = {}
    current_section = None

    with open(file_path, "r") as f:
        for line in f:
            line = line.strip()

            # Skip comments and empty lines
            if not line or line.startswith("#"):
                continue

            # Handle section headers
            if "{" in line and ":" not in line:
                current_section = line.split("{")[0].strip()
                config[current_section] = {}
                continue

            # Handle closing braces
            if line == "}":
                current_section = None
                continue

            # Handle key-value pairs
            if ":" in line:
                key, value = line.split(":", 1)
                converted_value = convert_value(value)

                if current_section:
                    config[current_section][key.strip()] = converted_value
                else:
                    config[key.strip()] = converted_value

    return config


def load_all_board_configs(spirom_dir: Path) -> Dict[str, BoardConfig]:
    """Load all board configurations from the spirom directory"""
    board_configs = {}

    for board_dir in spirom_dir.iterdir():
        if not board_dir.is_dir():
            continue

        board_name = board_dir.name
        configs = {}

        # Load each configuration file type
        for config_file in PB_FILES:
            if (config_path := board_dir / config_file).exists():
                file_type = config_file.removesuffix(".txt")
                configs[file_type] = parse_config_file(config_path)

        if configs:  # Only add boards with configurations
            board_configs[board_name] = BoardConfig(name=board_name, configs=configs)

    return board_configs


def get_all_keys_by_type(board_configs: Dict[str, BoardConfig]) -> Dict[str, set]:
    """Extract all unique keys organized by file type"""
    all_keys = defaultdict(set)

    for board_config in board_configs.values():
        for file_type in FILE_TYPES:
            config = board_config.get_config(file_type)
            extract_keys_recursive(config, all_keys[file_type])

    return all_keys


def extract_keys_recursive(
    config: Dict[str, Any], key_set: set, prefix: str = ""
) -> None:
    """Recursively extract all keys from nested configuration, filtering out ignored sections"""
    for key, value in config.items():
        # Skip ignored sections at the top level
        if not prefix and key in IGNORED_SECTIONS:
            continue

        full_key = f"{prefix}.{key}" if prefix else key

        if isinstance(value, dict):
            extract_keys_recursive(value, key_set, full_key)
        else:
            key_set.add(full_key)


def get_nested_value(config: Dict[str, Any], key_path: str) -> Any:
    """Get value from nested dictionary using dot notation"""
    current = config
    for key in key_path.split("."):
        if isinstance(current, dict) and key in current:
            current = current[key]
        else:
            return None
    return current


def format_display_value(value: Any, field_name: str) -> str:
    """Format a value for display in the documentation"""
    if value is None:
        return "-"
    elif isinstance(value, bool):
        return "✓" if value else "✗"
    elif isinstance(value, int) and field_name in HEX_FIELDS:
        return f"0x{value:X}"
    else:
        return str(value)


def generate_csv_table_row(
    full_key: str,
    display_key: str,
    file_type: str,
    board_configs: Dict[str, BoardConfig],
) -> str:
    """Generate a single CSV table row for a configuration parameter"""
    board_names = sorted(board_configs.keys())

    # Get values for all boards
    values = []
    for board_name in board_names:
        board_config = board_configs[board_name]
        config = board_config.get_config(file_type)
        value = get_nested_value(config, full_key)
        values.append(value)

    # Format row
    formatted_values = [format_display_value(v, full_key) for v in values]
    escaped_values = [f'"{v}"' for v in [display_key] + formatted_values]
    return "   " + ", ".join(escaped_values) + "\n"


def generate_overview_section(board_configs: Dict[str, BoardConfig]) -> str:
    """Generate overview section with board list"""
    return """Overview
========

This document provides board configurations across all TT BlackHole board variants.

"""


def generate_file_type_section(
    file_type: str, keys: set, board_configs: Dict[str, BoardConfig]
) -> str:
    """Generate a section for a specific file type"""
    title = SECTION_TITLES.get(file_type, file_type.title())
    board_names = sorted(board_configs.keys())

    # Group keys by section
    sections = defaultdict(list)
    standalone_keys = []

    for key in sorted(keys):
        if "." in key:
            section, subkey = key.split(".", 1)
            sections[section].append((key, subkey))
        else:
            standalone_keys.append(key)

    # Generate main section header
    content = f"\n{title}\n" + "=" * len(title) + "\n\n"

    # Add standalone keys table if any exist
    if standalone_keys:
        header_row = '"Configuration Parameter"' + "".join(
            f', "{board}"' for board in board_names
        )
        content += f""".. csv-table::
   :header: {header_row}
   :widths: auto

"""
        for key in standalone_keys:
            content += generate_csv_table_row(key, key, file_type, board_configs)
        content += "\n"

    # Create separate subsections for each section
    for section, section_keys in sorted(sections.items()):
        # Create subsection header
        section_title = section.replace("_", " ").title()
        content += f"{section_title}\n" + "-" * len(section_title) + "\n\n"

        # Create table for this section only
        header_row = '"Configuration Parameter"' + "".join(
            f', "{board}"' for board in board_names
        )
        content += f""".. csv-table::
   :header: {header_row}
   :widths: auto

"""

        for full_key, subkey in sorted(section_keys):
            content += generate_csv_table_row(
                full_key, subkey, file_type, board_configs
            )

        content += "\n"

    return content


def generate_rst_documentation(board_configs: Dict[str, BoardConfig]) -> str:
    """Generate complete RST documentation"""
    content = RST_HEADER
    content += generate_overview_section(board_configs)

    # Generate sections for each file type
    all_keys = get_all_keys_by_type(board_configs)

    for file_type in FILE_TYPES:
        if file_type in all_keys and all_keys[file_type]:
            content += generate_file_type_section(
                file_type, all_keys[file_type], board_configs
            )

    return content


def main() -> int:
    """Main entry point for the script"""
    try:
        # Use hardcoded paths relative to tt-system-firmware
        script_dir = Path(__file__).parent.parent
        spirom_dir = script_dir / "boards/tenstorrent/tt_blackhole/spirom_data_tables"
        output_path = (
            script_dir
            / "boards/tenstorrent/tt_blackhole/doc/_generated/tt_blackhole_configuration.rst"
        )

        if not spirom_dir.exists():
            print(f"Error: Could not find spirom_data_tables directory at {spirom_dir}")
            return 1

        print(f"Loading board configurations from: {spirom_dir}")

        # Load all board configurations
        board_configs = load_all_board_configs(spirom_dir)

        if not board_configs:
            print("Warning: No board configurations found")
            return 1

        print(f"Found {len(board_configs)} board configurations:")
        for board_name in sorted(board_configs.keys()):
            print(f"  - {board_name}")

        # Generate documentation
        output_path.parent.mkdir(parents=True, exist_ok=True)

        print(f"Generating RST documentation: {output_path}")
        rst_content = generate_rst_documentation(board_configs)
        output_path.write_text(rst_content)

        print(f"RST documentation generated successfully: {output_path.absolute()}")
        print(
            "File is automatically included in the documentation build via boards/tenstorrent/tt_blackhole/doc/index.rst"
        )
        return 0

    except Exception as e:
        print(f"Unexpected error: {e}")
        return 1


if __name__ == "__main__":
    exit(main())
