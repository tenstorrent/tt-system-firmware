# v19.16.1

We are pleased to announce the release of TT System Firmware version 19.16.1 🥳🎉.

This is a patch release on top of v19.16.0. It includes the following updates:
- Disabling MRISC powerdown policy on Galaxy systems to resolve GDDR issues
  - As a result, Galaxy idle power increases by 1.6 kW

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

### Ethernet

- Manual EQ now recovers when the link partner loses signal mid-training
  (e.g. partner reset) instead of failing; removed the 2 s post-sigdet.
- Fixed retrain and train_status bugs (successful retrain treated as a
  failure, loopback statuses overwritten).

### PCIe

- Request per-rate PCIe EQ presets from the firmware table.
- Request different presets on Galaxy systems vs PCIe cards (SYS-5101).

## Boot & Firmware

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
