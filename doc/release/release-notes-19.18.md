# v19.18.0

> This is a working draft for the up-coming 19.18.0 release.

We are pleased to announce the release of TT System Firmware version 19.18.0 🥳🎉.

Major enhancements with this release include:

## What's Changed

## Wormhole

## Blackhole

### PCIe

- Advertise 2 MSI vectors (Multiple Message Capable = 1) at every PCIe init, including after a chip reset. Vector 1 is for HDMA completions, which the host driver owns; firmware sends its own MSIs on vector 0 only.
- Set HDMA channel 0's PF_DEPTH to 7 for the read and write engines at PCIe init. Channel 0's value applies to every channel, so channels 0 and 1 get 8 prefetch entries each. Firmware's DMA messages stay on channel 0; channel 1 is left to the host driver.

### Telemetry

- Report the HDMA change in telemetry: bit 3 (`hdma_host_channel_1`) of `TAG_FW_CAPABILITIES_0` and `TAG_FW_ACTIVE_CONFIG_0`.

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

An overview of required and recommended changes to make when migrating from the previous v19.17.0 release can be found in [19.18 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.18.md).

## Full ChangeLog

The full ChangeLog from the previous v19.17.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.17.0...v19.18.0
