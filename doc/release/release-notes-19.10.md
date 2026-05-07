# v19.10.0

> This is a working draft for the up-coming 19.10.0 release.

We are pleased to announce the release of TT System Firmware version 19.10.0 🥳🎉.

Major enhancements with this release include:

## What's Changed

## Wormhole

### Power & Performance Improvements
- Link AICLK_BUSY to the `max_ai_clk` bit of the power message
- Disable DRAM low power on instances that are slow to retrain
- Update msg ID for `POWER_SETTING` message to 0xC0 and mark 0xBF as reserved. This prevents KMD from enabling this feature on previous FWs.

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

An overview of required and recommended changes to make when migrating from the previous v19.9.0 release can be found in [19.10 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.10.md).

## Full ChangeLog

The full ChangeLog from the previous v19.9.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.9.0...v19.10.0
