# v19.15.0

> This is a working draft for the up-coming 19.15.0 release.

We are pleased to announce the release of TT System Firmware version 19.15.0 🥳🎉.

Major enhancements with this release include:

## What's Changed

## Blackhole

### Boot Filesystem

- The boot filesystem is now split across three descriptor tables so that a firmware update no longer writes the flash sectors holding the images the board needs in order to boot at all. The records that do not change after manufacturing (MCUBoot, the recovery image and its trailer, and board configuration) stay in the ROM table at `0x0`, the failover MCUBoot record gets a table of its own at `0x4000`, and everything an update rewrites moves to a new mutable table at `0x170000`. A header at `0x120000` lists where the tables are; the SMC ROM does not read it and still goes to the fixed `0x0` and `0x4000` addresses.
  - **REQUIRES** `tt-flash` v3.11.0+ to get the benefit. It compares each boot-critical image against the one already on the chip and keeps both the image and its descriptor when they match, which is what leaves those sectors unerased. The comparison is of image content rather than raw bytes, because signing is not reproducible and two builds of the same source differ in their signature. Older `tt-flash` still flashes a 19.15.0 bundle correctly, but rewrites every segment it carries as it always has, so an update stays exposed to a power loss corrupting the fallback boot path.
  - The recovery image trailer now ships with its `copy-done` flag already set. MCUBoot wrote that byte itself on the first boot into recovery, which left the trailer on flash a byte away from the one in the bundle and had it rewritten by every later update.

## Migration guide

An overview of required and recommended changes to make when migrating from the previous v19.14.0 release can be found in [19.15 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.15.md).

## Full ChangeLog

The full ChangeLog from the previous v19.14.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.14.0...v19.15.0
