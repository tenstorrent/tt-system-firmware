# v19.15.0

We are pleased to announce the release of TT System Firmware version 19.15.0 🥳🎉.

Major enhancements with this release include:

- Second-source SPI flash support on Blackhole Galaxy, with a flash-tool interlock so an image that cannot drive the fitted EEPROM cannot be written.
- Galaxy bin6 board revision (`0x202`), for UBB parts that permit one harvested DRAM instance.
- Persistent `bh-mod` override of per-instance PCIe max generation.
- Fix of inverted Tensix clock-gate enable: `feature_enable.cg_en` now actually enables clock gating (lower idle / light-load Tensix power on production tables).

## What's Changed

## Blackhole

### Second-source SPI Flash

Blackhole Galaxy boards may now ship a second-source SPI EEPROM instead of the original Micron MT25QU512ABB. Firmware selects the matching flash configuration at boot from a mux of known parts, and reports the fitted JEDEC ID in telemetry.

Supported parts (JEDEC ID as `0x00MMTTCC`):

| JEDEC ID   | Part |
| ---------- | ---- |
| `0x20bb20` | Micron MT25QU512ABB |
| `0xc2253a` | Macronix MX25U51245G |
| `0xc8631a` | GigaDevice GD25LF512MF |
| `0xef6020` | Winbond W25Q51RW-Q/-N, W25Q512NW-IQ/IN |

An unrecognized chip still comes up on a single-I/O fallback so the board remains readable and reflashable.

Writing an MT25-only image onto a second-source board would leave firmware that cannot read itself back, including recovery. This release therefore adds a three-way interlock between the firmware bundle, tt-flash, and the running firmware:

1. Each image in the bundle carries a `compat-variables.json` listing the board variables it constrains and the values it supports. The first variable is SPI EEPROM JEDEC ID (`BOARD_VAR_SPI_JEDEC_ID`, telemetry tag 80).
2. tt-flash compares those constraints to the value reported by the running firmware and refuses an incompatible image.
3. `TT_SMC_MSG_FLASH_UNLOCK` carries a bitmap of the variables tt-flash verified. Firmware refuses to unlock if a required variable was not checked, so an older tt-flash that does not know about board variables cannot write a second-source board.

Original Micron MT25 boards are exempt from the JEDEC-ID requirement, so existing tt-flash continues to work on those boards. Second-source boards require a tt-flash that implements the interlock and a 19.15 (or later) bundle. See the [19.15 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.15.md) and the [board variables documentation](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/services/board_variables/index.rst).

### Boards

- New board revision: Galaxy bin6 (`tt_blackhole@galaxy_bin6`, board type `0x202`). SPI config is based on Galaxy Rev C with `product_spec_harvesting.dram_disable_count` set to 1 (one harvested DRAM instance). The SMC overlay reuses Rev C GDDR parameters.

### Persistent SPI Flash Parameters

- Expose `pci0_property_table.max_pcie_speed` and
  `pci1_property_table.max_pcie_speed` to `bh-mod`, valid values
  `{0, 1, 2, 3, 4, 5}`. `0` is unconstrained (Gen 5 default).
  `bh-mod res` restores the cmfwcfg value. Set the instance
  whose `pcie_mode` is EP.

### Power

- Fix accidentally inverted Tensix clock-gate enable. `EnableTensixCG()` now applies clock gating when `feature_enable.cg_en` is set, and skips it when the flag is clear. Every production firmware table ships `cg_en: true`, so Tensix clock gating takes effect on upgrade. Expect lower idle and light-load Tensix power; full-load draw is largely unchanged.

### Telemetry

- Publish the SPI flash JEDEC ID as `TAG_FLASH_JEDEC_ID` (tag 80), packed as `0x00MMTTCC` (`MM` manufacturer, `TT` memory type, `CC` capacity). `0` if the ID could not be read.
- Add characterization submessage `TT_SUB_MSG_SET_TELEMETRY_UPDATE_INTERVAL` to set the periodic telemetry interval (10–1000 ms; `0` restores the 100 ms default).

### Logging

- Publish error-level logs to the host (KMD) immediately rather than waiting for the next periodic flush.
- Drain queued logs when a GDDR thermal trip is processed, so the trip context is visible on the host.

### Host Interface

- `TT_SMC_MSG_FLASH_UNLOCK` now accepts a verified-board-variable bitmap. The reply always reports the variables this board requires in `data[1]`. See `flash_unlock_rqst` in `msgqueue.h`.
- Board type extracted from `board_id` is a `uint32_t` (was narrower), covering up to 5-hex-digit types such as Galaxy bin6.

### Drivers

- Add a flash mux that probes JEDEC-ID-matched candidate configurations at boot and selects the first that accepts the fitted chip.
- Drive Tensix and Ethernet tile/RISC resets through the Zephyr reset API, with complete Blackhole reset DT coverage.
- DMC: initial MCTP/PLDM support.

### Tooling

- Blackhole recovery flashloader probe table adds Macronix MX25U51245G, GigaDevice GD25LF512MF, and Winbond W25Q51RW.
- Remove the unused `tt-console` binary from tooling.
- SMC pytest waits 0.7 s after a power-state toggle and polls the power delta after the low-power settle, so the power-state tests are less timing-sensitive.

### Documentation

- Document board variables and the flash interlock under `doc/services/board_variables/`.

## Grendel

- New SoC/board bring-up: `tt_keraunos`, `tt_mmk`, Mimir remote boot, and K-SMC-bl0p5 (with BL0p5 logging).
- Msgqueue gains a mailbox IRQ doorbell backend, used by MMK.
- Register-name shim for Keraunos.

## Migration guide

An overview of required and recommended changes to make when migrating from the previous v19.14.0 release can be found in [19.15 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.15.md).

## Full ChangeLog

The full ChangeLog from the previous v19.14.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.14.0...v19.15.0
