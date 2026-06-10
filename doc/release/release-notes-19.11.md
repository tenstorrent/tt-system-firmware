# v19.11.0

We are pleased to announce the release of TT System Firmware version 19.11.0 🥳🎉.

---

## General

- Update to Zephyr 4.4.0 for Blackhole and Grendel

## Blackhole

### Firmware Init Error Reporting

- The `STATUS_ERROR_STATUS0` scratch register now reports SMC init failures as a bit field, with one bit per init stage. Bits accumulate, so a single read can indicate multiple failures.

  | Bit | Stage          |
  | --- | -------------- |
  | 0   | Regulator init |
  | 1   | Cable fault    |
  | 2   | Tensix init    |
  | 3   | MRISC load     |
  | 4   | GDDR training  |

  For example, `0b00000101` means both regulator init and Tensix init failed on this boot.

### Persistent SPI Flash Parameters

- Selected firmware table parameters can now be overridden at runtime via `bh-mod`, and the override persists across firmware upgrades.
  - **REQUIRES** `tt-flash` v3.8.0+ or else your configuration won't persist across firmware upgrades. No other negative effects when using older `tt-flash`.
  - Only `chip_limits.tdp_limit` is exposed; additional parameters can be exposed in future releases as use cases arise.

### PCIe

- For P300C boards, update PCIe speed to Gen4 to improve QB2 PCIe stability (SYS-4229).

### Ethernet

- Updated Blackhole ERISC FW to v1.11.0
  - Keep all ETH interrupt modes DISABLED to avoid jumping into the IVT on RISC0 kernel switchover (tt-metal#44188)
  - Enable MACPCS ECC cabability
  - Added MACPCS/SerDes reset-deassert timestamps to boot results
  - ETH msg PORT_STATUS_CLEAR: clear accumulated live status counts in L1

---

## Grendel

None.

---

## Wormhole

None.

---

## Migration guide

An overview of required and recommended changes to make when migrating from the previous v19.10.0 release can be found in [19.11 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.11.md).

## Full ChangeLog

The full ChangeLog from the previous v19.10.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.10.0...v19.11.0
