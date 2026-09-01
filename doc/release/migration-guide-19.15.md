# 19.15.0

## Migration Guide

This document lists recommended and required changes for those migrating from the previous v19.14.0 firmware release to the new 19.15.0 firmware release.

### tt-flash 3.11.0 or later

Update `tt-flash` before flashing this release:

```
pip install 'tt-flash>=3.11.0'
```

19.15.0 splits the Blackhole boot filesystem across three descriptor tables, and 3.11.0 is the first release that reads that layout. It is also the first that leaves MCUBoot and the recovery image alone when the chip already holds them, which is what stops a routine update from erasing the flash sectors the board boots from.

Older versions will still flash a 19.15.0 bundle, and the board will come up on it. They rewrite every segment the bundle carries, boot-critical images included, so an update remains vulnerable to a power loss corrupting the fallback boot path -- the behaviour of every release up to this one. Nothing else about flashing changes.
