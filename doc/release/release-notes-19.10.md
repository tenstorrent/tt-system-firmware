# v19.10.0

> This is a working draft for the up-coming 19.10.0 release.

We are pleased to announce the release of TT System Firmware version 19.10.0 🥳🎉.

Major enhancements with this release include:

## What's Changed

## General

### Documentation

  * Add documentation for testing the release process on a personal fork.

## Wormhole

### Power & Performance Improvements
- Link AICLK_BUSY to the `max_ai_clk` bit of the power message
- Disable DRAM low power on instances that are slow to retrain
- Update msg ID for `POWER_SETTING` message to 0xC0 and mark 0xBF as reserved. This prevents KMD from enabling this feature on previous FWs.

## Blackhole

### New API

  * Added [`TT_SMC_MSG_TOGGLE_ETH_RESET`](https://docs.tenstorrent.com/tt-system-firmware/doxygen/structeth__tile__reset__rqst.html) message to reset ethernet tiles (SYS-4228).

### Documentation

  * Autogenerate board-specific documentation from protobuf definitions.

### Power and voltage

  * Get `VDD_MIN` and `VDD_MAX` from the fw_table

### Fan control and telemetry

  * Disable the MAX6639 fan controller on boards that have no fan (relevant P150/P300 DMC overlays) so the DMC does not report telemetry for a non-present fan.
  * When fan control is disabled, report fan speed and RPM telemetry as invalid (`0xffffffff`).

### CI

  * Add Loudbox to CI, running nightly soak and Metal tests

## Grendel

No Grendel changes.

## Migration guide

An overview of required and recommended changes to make when migrating from the previous v19.9.0 release can be found in [19.10 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.10.md).

## Full ChangeLog

The full ChangeLog from the previous v19.9.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.9.0...v19.10.0
