# v19.16.0

We are pleased to announce the release of TT System Firmware version 19.16.0 🥳🎉.

Major enhancements with this release include:

- New Blackhole board revision: Galaxy CF
- Enhanced boot firmware table layout for improved robustness during firmware updates.

## What's Changed

## Blackhole

### Boards

- New board revision: Galaxy CF (`tt_blackhole@galaxy_cf`, board type `0x202`).

### Reliability

- Added GDDR CA latch-and-retest functionality (SYS-5042) to improve GDDR initialization reliability on Blackhole.

## Boot & Firmware

### Boot Filesystem

- Enhanced static firmware table layout with multi-table organization for improved robustness.
- Moved mutable firmware records to separate table to reduce risk of corruption during updates.

### MCUBoot

- Derived MCUBoot RAM load window from device tree configuration for improved flexibility.

### Drivers

- **Serial/VUART**: Implemented placeable VUART driver for flexible virtual UART placement.
- **Remoteproc**: Split remoteproc load and execute operations for better control and test isolation.

## Libraries

- **ARC Library**: Added DVFS loop counters (dropped ticks, maximum period, and maximum pass duration) to support power analysis.
- **FW Common**: Enabled TT_FW_COMMON library compilation for mission FW.

## Bug Fixes & Improvements

- Fixed typos in code comments and Doxygen markers throughout the codebase.
- Fixed SPI-NOR flash (nv_flash) size configuration.

## Migration guide

An overview of required and recommended changes to make when migrating from the previous v19.15.0 release can be found in [19.16 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.16.md).

## Full ChangeLog

The full ChangeLog from the previous v19.15.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.15.0...v19.16.0
