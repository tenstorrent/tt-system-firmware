# v19.15.0

> This is a working draft for the up-coming 19.15.0 release.

We are pleased to announce the release of TT System Firmware version 19.15.0 🥳🎉.

Major enhancements with this release include:

## What's Changed

## Blackhole

### Persistent SPI Flash Parameters
- Expose `pci0_property_table.max_pcie_speed` and
  `pci1_property_table.max_pcie_speed` to `bh-mod`, valid values
  `{0, 1, 2, 3, 4, 5}`. `0` is unconstrained (Gen 5 default).
  `bh-mod res` restores the cmfwcfg value. Set the instance
  whose `pcie_mode` is EP.

## Migration guide

An overview of required and recommended changes to make when migrating from the previous v19.14.0 release can be found in [19.15 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.15.md).

## Full ChangeLog

The full ChangeLog from the previous v19.14.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.14.0...v19.15.0
