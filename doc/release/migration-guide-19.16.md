# 19.16.0

## Migration Guide

This document lists recommended and required changes for those migrating from the previous v19.15.0 firmware release to the new 19.16.0 firmware release.

## Required: Galaxy CF board type

- **Galaxy CF** (board type `0x202`): If your board type is a GALAXY-CF, you must use a tt-flash version that supports it. (These notes will be updated with the minimum tt-flash version number once it is released).

## Galaxy CF

Galaxy CF is a new UBB board type (`0x202`, `tt_blackhole@galaxy_cf`). Host tools that switch on board type need to treat `0x202` as a Galaxy/UBB variant.
