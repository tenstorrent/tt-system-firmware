# v19.14.0

> This is a working draft for the up-coming 19.14.0 release.

We are pleased to announce the release of TT System Firmware version 19.14.0 🥳🎉.

Major enhancements with this release include:

## What's Changed

## Blackhole

### KMD Logging
- Enable the KMD firmware logging backend by default on Blackhole SMC.

### Ethernet

- Updated Blackhole ERISC FW to v1.12.2, change list from v1.12.0:
  - v1.12.2 (2026-08-10)
    - Updated auto train train speed (speed=0) for UBB to 330G for chip-to-chip
      and chip-to-examax-passive ports, keep retimers and QSFPs at 200G

  - v1.12.1 (2026-07-15):
    - Updated the default AW SerDes FW to v0.9.17, which now has the 90G, 95G, and 100G lane speeds built into the default image
    - Expanded SerDes speed loading logic for new speeds to work on all ports except the QSFP and retimer ports
    - The default firmware build skips the retimer ports until a BMC release is available that can program them

- Removed alternative SerDes firmware support from the SMC. The `altsdfw` and `altsdreg` flash partitions and their blobs have been dropped, as the required link speeds are now part of the default SerDes firmware image.

## Migration guide

An overview of required and recommended changes to make when migrating from the previous v19.13.0 release can be found in [19.14 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.14.md).

## Full ChangeLog

The full ChangeLog from the previous v19.13.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.13.0...v19.14.0
