# v19.12.0

> This is a working draft for the up-coming 19.12.0 release.

We are pleased to announce the release of TT System Firmware version 19.12.0 🥳🎉.

Major enhancements with this release include:

## What's Changed

## Wormhole

## Blackhole

### Power & Performance Improvements
- Enable process based V/F curve for p300c
- Read GDDR temperatures more frequently as part of internal telemetry
- Add Blackhole Kernel throttler when we reach AICLK floor (disabled by default)
- Add throttler controls via ARC message to enable/disable kernel throttling at AICLK floor and tune the stop-NOPs frequency threshold (kernel throttling at floor is now disabled by default).
- Get the kernel throttler defaults (enable and stop-NOPs frequency) from the firmware table so they persist across reboots and firmware upgrades, and can be overridden via `bh-mod`. The ARC message continues to override these defaults at runtime.

### Telemetry
- Carve out a region of memory for Metal runtime telemetry and publish its address and size via `SCRATCH_RAM[22:23]`.
- Add `TAG_KERNEL_THROTTLER` telemetry reporting the active kernel-throttler-at-AICLK-floor configuration (enable bit and stop-NOPs frequency).

### GDDR
- Change NOC endpoint used to load GDDR0 MRISC from 0 to 2 (NOC node 0-11)
- Report the NOC endpoint each GDDR instance loads MRISC on in telemetry at `TAG_GDDR_MRISC_NOC2AXI_PORT`

### Ethernet

- Updated Blackhole ERISC FW to v1.12.0
  - Added support for alternative SerDes link speeds (95G, 106G)
  - Reworked runtime link check and recovery logic
    - Replaced recovery branches with an exponential-backoff escalator: SerDes retrain -> full reinit -> bring port down
    - Added port-down auto-recovery via a sustained-sigdet counter; link-from port down now only happens during retrains
  - Refactored eth_api_table so every API has a wrapper that records call count, last call timestamp, and time spent in function

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

An overview of required and recommended changes to make when migrating from the previous v19.11.0 release can be found in [19.12 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.12.md).

## Full ChangeLog

The full ChangeLog from the previous v19.11.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.11.0...v19.12.0
