# 19.15.0

## Migration Guide

This document lists recommended and required changes for those migrating from the previous v19.14.0 firmware release to the new 19.15.0 firmware release.

## Required: second-source SPI flash interlock

Blackhole Galaxy may now ship a second-source SPI EEPROM. Firmware older than 19.15 only speaks the Micron MT25QU512ABB command set. Writing that image onto a second-source board leaves firmware that cannot read itself back, including recovery.

19.15 adds an interlock so tt-flash will not write an image that cannot support the board in front of it. There are three participants:

1. **Firmware bundle** — each image declares the board variables it knows and the values it supports, in `compat-variables.json`.
2. **tt-flash** — checks those values against the running firmware (today: SPI EEPROM JEDEC ID from telemetry tag 80) and refuses an incompatible image.
3. **Running firmware** — `FLASH_UNLOCK` carries a bitmap of the variables tt-flash checked. Firmware refuses to unlock if a required variable is missing, so an older tt-flash that does not know about board variables cannot flash a second-source board.

The first board variable is SPI EEPROM JEDEC ID (`BOARD_VAR_SPI_JEDEC_ID` = 0).

### What you must do

- **Original Micron MT25QU512ABB boards** (`JEDEC ID 0x20bb20`): no tool change is required. Firmware exempts this part from the JEDEC-ID unlock check, so existing tt-flash continues to work.
- **Second-source SPI boards** (any other JEDEC ID): you need both
  - a 19.15.0 (or later) firmware bundle, which includes `compat-variables.json`, and
  - a tt-flash that implements the board-variable interlock ([tt-flash#112](https://github.com/tenstorrent/tt-flash/pull/112)).
- **Galaxy bin6** (board type `0x202`): tt-flash 3.11.0 or later, in addition to the interlock above if the board is second-source.

Until tt-flash#112 is in a numbered release, flashing a second-source board requires a tt-flash build that includes that change.

### Compatibility matrix

`x` means the version of that participant does not matter. "new" means 19.15 firmware / a bundle that carries `compat-variables.json` / a tt-flash that implements the interlock. Observed messages are from qualification.

| fwbundle | tt-flash | running FW | Result | Observed |
| -------- | -------- | ---------- | ------ | -------- |
| x | x | old | Not applicable / don't care | (old firmware does not implement the interlock) |
| x | old | new | No app support; firmware must not unlock | `Error: Failed to unlock spi` |
| old | new | new | Bundle does not declare hardware support; firmware must not unlock | `Error: The firmware on this board will not allow a flash until these are verified against the image: board variable 0. This firmware bundle does not say which hardware it supports; use a newer bundle.` |
| new but incompatible | new | new | tt-flash rejects before write | `Error: This firmware is not compatible with this board: SPI EEPROM is 0x20bb20; this firmware supports 0x20bb21` |
| new | new | new | Flash unlocks; new image is written | successfully flashed |

The incompatible-bundle example above is the error shape (actual JEDEC IDs depend on the fitted part and the image).

### Supported SPI EEPROM JEDEC IDs

19.15 images that include the Galaxy flash mux accept:

| JEDEC ID   | Part |
| ---------- | ---- |
| `0x20bb20` | Micron MT25QU512ABB |
| `0xc2253a` | Macronix MX25U51245G |
| `0xc8631a` | GigaDevice GD25LF512MF |
| `0xef6020` | Winbond W25Q51RW-Q/-N, W25Q512NW-IQ/IN |

An unrecognized chip still enumerates on a single-I/O fallback so the card stays readable. Flashing it still requires a bundle that lists that JEDEC ID. `tt-flash --force` does not override this check.

### Downgrade

On a **second-source** board that is already running 19.15:

- Old tt-flash cannot unlock the flash (`Failed to unlock spi`).
- A new tt-flash will not write a pre-19.15 bundle, because that bundle has no `compat-variables.json`.

You cannot return those boards to pre-19.15 firmware with the old tools. Stay on 19.15+ bundles and an interlock-aware tt-flash.

On an **MT25** board, existing tools and older bundles continue to work.

See [board variables](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/services/board_variables/index.rst) for the protocol.

## Recommended: Tensix clock gating now honors `cg_en`

`feature_enable.cg_en` was inverted: firmware skipped Tensix clock gating when the flag was set, and applied it when the flag was clear. 19.15 corrects this.

Every production firmware table ships `cg_en: true`, so after upgrade Tensix clock gating is on. Idle and light-load Tensix power should drop; full-load power is largely unchanged. If you had cleared `cg_en` to *enable* gating under the old inverted logic, that board will now run with gating off — set `cg_en` to the intended polarity.

## Optional: PCIe max generation override

`pci0_property_table.max_pcie_speed` and `pci1_property_table.max_pcie_speed` can be overridden with `bh-mod` and persist across firmware upgrades. Valid values are `{0, 1, 2, 3, 4, 5}`; `0` is unconstrained (Gen 5 default). `bh-mod res` restores the cmfwcfg value. Set the instance whose `pcie_mode` is EP. This requires tt-flash 3.8.0 or later to preserve `ccfgovr` across flashes.

## Galaxy bin6

Galaxy bin6 is a new UBB board type (`0x202`, `tt_blackhole@galaxy_bin6`). Host tools that switch on board type need to treat `0x202` as a Galaxy/UBB variant (one harvested DRAM instance; SPI tables based on Galaxy Rev C). tt-flash 3.11.0 adds this type.
