# v19.13.0

> This is a working draft for the up-coming 19.13.0 release.

We are pleased to announce the release of TT System Firmware version 19.13.0 🥳🎉.

Major enhancements with this release include:

## What's Changed

## Wormhole

### Telemetry
- Carve out a region of memory for Metal runtime telemetry and publish its address and size via `SCRATCH_EXT[0:1]`
  - Add `SCRATCH_EXT` as a fixed-address carve out of CSM for extra "scratch registers"

## Blackhole

### Telemetry
- Add telemetry reporting feature capabilities, distinguishing which features the firmware is capable of from which are currently enabled.
- Add a `bh_arc` telemetry sensor driver for the DMC, exposing SMC telemetry through the standard Zephyr sensor API.

### KMD Logging
- Add a KMD logging backend that streams firmware log messages to a KMD-allocated buffer over PCIe (see the [KMD logging service documentation](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/services/kmd_logging/index.rst)). This backend is disabled by default on all builds and must be enabled via `CONFIG_TT_PCIE_LOG_BACKEND`.

### GDDR Thermal
- Move the GDDR thermal-trip catmon trigger behind a Kconfig (disabled by default), while keeping DMC logging of the event.

### Ethernet
- Gate loading of the alternative SerDes configuration (UBB boards) on the Ethernet speed override: it now only loads when no override is set or the override is 400G, and is skipped for other speed overrides.

### Stability Improvements
- Fix an intermittent Tensix reset hang by using NOC coordinates instead of physical coordinates when clock-gating a single tile.

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

An overview of required and recommended changes to make when migrating from the previous v19.12.0 release can be found in [19.13 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.13.md).

## Full ChangeLog

The full ChangeLog from the previous v19.12.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.12.0...v19.13.0
