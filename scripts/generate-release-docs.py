#!/usr/bin/env python3

# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

# Script to generate release notes and migration guide files for a firmware version
# Based on the template format from commit b953481ccec15fa45524ab4c7da8635eeb143a3a

import sys
from pathlib import Path


def find_project_root() -> Path:
    """Walk up from script location to find directory containing VERSION file."""
    script_dir = Path(__file__).resolve().parent
    current = script_dir

    while current != current.parent:
        version_file = current / "VERSION"
        if version_file.exists():
            return current
        current = current.parent

    raise RuntimeError(
        "Could not find project root (directory containing VERSION file)"
    )


def parse_version_file(version_file: Path) -> dict:
    """Parse VERSION file and return version components as a structured dict."""
    raw_parts = {}

    with open(version_file, "r") as f:
        for line in f:
            if "=" in line:
                key, value = line.strip().split("=", 1)
                key = key.strip()
                value = value.strip()
                raw_parts[key] = value

    return {
        "major": raw_parts.get("VERSION_MAJOR", "0"),
        "minor": raw_parts.get("VERSION_MINOR", "0"),
        "patch": raw_parts.get("PATCHLEVEL", "0"),
        "extra": raw_parts.get("EXTRAVERSION", ""),
    }


def get_version(version_file: Path) -> str:
    """Get version string with 'v' prefix and optional extraversion."""
    parts = parse_version_file(version_file)
    major = parts["major"]
    minor = parts["minor"]
    patch = parts["patch"]

    return f"v{major}.{minor}.{patch}"


def next_release_version_base(version_file: Path) -> str:
    """Get the next release version string."""
    parts = parse_version_file(version_file)
    major = parts["major"]
    minor = str(int(parts["minor"]) + 1)
    patch = 0

    return f"{major}.{minor}.{patch}"


def next_release_version(version_file: Path) -> str:
    """Get the next release version string."""

    return f"v{next_release_version_base(version_file)}"


def previous_release_version(version_file: Path) -> str:
    """Get the previous release version string."""
    parts = parse_version_file(version_file)
    major = parts["major"]
    minor = parts["minor"]
    patch = 0

    return f"v{major}.{minor}.{patch}"


def get_version_base(version_file: Path) -> str:
    """Get version string without 'v' prefix and without extraversion."""
    parts = parse_version_file(version_file)
    major = parts["major"]
    minor = parts["minor"]
    patch = parts["patch"]

    return f"{major}.{minor}.{patch}"


def generate_release_notes(
    output_file: Path,
    next_version: str,
    next_version_base: str,
    previous_version: str,
    version_major_minor: str,
) -> None:
    """Generate release notes file."""
    content = f"""# {next_version}

> This is a working draft for the up-coming {next_version_base} release.

We are pleased to announce the release of TT System Firmware version {next_version_base} 🥳🎉.

Major enhancements with this release include:

## What's Changed

## Wormhole

## Blackhole

## Grendel

<!-- Subsections can break down improvements by (area or board) -->
<!-- UL PCIe -->
<!-- UL DDR -->
<!-- UL Ethernet -->
<!-- UL Telemetry -->
<!-- UL Debug / Developer Features -->
<!-- UL Drivers -->
<!-- UL Libraries -->

<!-- Performance Improvements, if applicable -->
<!-- New and Experimental Features, if applicable -->
<!-- External Project Collaboration Efforts, if applicable -->
<!-- Stability Improvements, if applicable -->
<!-- Security vulnerabilities fixed? -->
<!-- API Changes, if applicable -->
<!-- Removed APIs, H3 Deprecated APIs, H3 New APIs, if applicable -->
<!-- New Samples, if applicable -->
<!-- Other Notable Changes, if applicable -->
<!-- New Boards, if applicable -->

## Migration guide

An overview of required and recommended changes to make when migrating from the previous {previous_version} release can be found in [{version_major_minor} Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-{version_major_minor}.md).

## Full ChangeLog

The full ChangeLog from the previous {previous_version} release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/{previous_version}...{next_version}
"""

    output_file.write_text(content)


def generate_migration_guide(
    output_file: Path,
    current_version_base: str,
    previous_version: str,
) -> None:
    """Generate migration guide file."""
    content = f"""# {current_version_base}

## Migration Guide

> This is a working draft for the up-coming {current_version_base} release.

This document lists recommended and required changes for those migrating from the previous {previous_version} firmware release to the new {current_version_base} firmware release.
"""

    output_file.write_text(content)


def main() -> int:
    try:
        project_root = find_project_root()
    except RuntimeError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    version_file = project_root / "VERSION"
    if not version_file.exists():
        print(f"Error: VERSION file not found at {version_file}", file=sys.stderr)
        return 1

    # Determine output directory
    output_dir = project_root / "doc" / "release"

    # Get current version
    next_version_base = next_release_version_base(version_file)
    next_version = next_release_version(version_file)
    previous_version = previous_release_version(version_file)

    # Extract major.minor for filename
    version_major_minor = ".".join(next_version_base.split(".")[:2])

    # Create output directory if it doesn't exist
    output_dir.mkdir(parents=True, exist_ok=True)

    # Generate release notes file
    release_notes_file = output_dir / f"release-notes-{version_major_minor}.md"
    generate_release_notes(
        release_notes_file,
        next_version,
        next_version_base,
        previous_version,
        version_major_minor,
    )
    print(f"Generated: {release_notes_file}")

    # Generate migration guide file
    migration_guide_file = output_dir / f"migration-guide-{version_major_minor}.md"
    generate_migration_guide(
        migration_guide_file,
        next_version_base,
        previous_version,
    )
    print(f"Generated: {migration_guide_file}")

    print()
    print("Release documentation files generated successfully!")
    print(f"  Release notes: {release_notes_file}")
    print(f"  Migration guide: {migration_guide_file}")
    print()
    print("Next steps:")
    print("  1. Commit the files:")
    print(f"     git add {release_notes_file} {migration_guide_file}")
    print(
        f'     git commit -sm "doc: release: {version_major_minor}: add draft release notes"'
    )
    print("  2. Post a PR with the generated files and merge after CI is successful.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
