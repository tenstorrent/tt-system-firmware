# v19.17.0

We are pleased to announce the release of TT System Firmware version 19.17.0 🥳🎉.

Major enhancements with this release include:

## What's Changed

## Blackhole

### Persistent SPI Flash Parameters

- Add `feature_enable.bypass_cable_fault_detection` to the firmware table and
  expose it to `bh-mod`. When set, the SMC skips the boot-time cable fault
  check (the 12V-2x6 power limit the DMC reports), so a 0 W limit no longer
  puts the chip into low-power cable fault mode. Off in every board table;
  intended for bring-up and debug. `bh-mod res` restores the cmfwcfg value.

## Migration guide

An overview of required and recommended changes to make when migrating from the previous v19.16.0 release can be found in [19.17 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.17.md).

## Full ChangeLog

The full ChangeLog from the previous v19.16.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.16.0...v19.17.0
