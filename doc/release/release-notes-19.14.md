# v19.14.0

We are pleased to announce the release of TT System Firmware version 19.14.0 🥳🎉.

Major enhancements with this release include:

## What's Changed

## General

- Restructure the release process page, move the version bump on main to immediately follow branch creation, and describe the tag-driven draft release and the pull request workflow required by protected release branches.

## Blackhole

### Logging
- Enable the KMD firmware logging backend by default on Blackhole SMC.
- Stop forwarding the boot banner to the KMD log; it carried no useful information and cluttered the output.
- The DMC application no longer calls ring buffer log backend functions when `CONFIG_LOG_BACKEND_RINGBUF` is not selected, which previously failed to link.

### GDDR Thermal Trip
- Move the GDDR thermal-trip (CATMON) configuration out of Kconfig and compile-time constants and into the firmware table:
  - `chip_limits.gddr_therm_trip_temp`, `chip_limits.gddr_therm_trip_critical_temp` and `chip_limits.gddr_therm_trip_duration_min` carry the thresholds.
  - `feature_enable.gddr_therm_trip_en` gates the action, and ships disabled on every board.
  - `CONFIG_TT_BH_ARC_GDDR_THERM_TRIP_ACTION*` is removed.
- Add the `TT_SUB_MSG_SET_GDDR_THERM_TRIP_ENABLED` characterization submessage so the host can enable or disable the trip action at runtime.
- Report the feature in telemetry: capability bit 1 (`gddr_therm_trip`) of `TAG_FW_CAPABILITIES_0` and the matching enable bit of `TAG_FW_ACTIVE_CONFIG_0`.

### GDDR Harvesting
- Add `dram_table.soft_harvest_dram_mask` to the firmware table. Each set bit clears the corresponding GDDR channel from the enabled tile set, which the product-spec `dram_disable_count` path could not express. The field defaults to `0` (no harvesting) when a board's firmware table omits it.

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
- The Ethernet reset path now loads the SerDes configuration as well, matching `EthInit()`, and reports SerDes lookup and load failures through four new `eth_reset` error codes: `ETH_RESET_ERR_SERDES_FW_LOOKUP` (9), `ETH_RESET_ERR_SERDES_FW_LOAD` (10), `ETH_RESET_ERR_SERDES_CFG_LOOKUP` (11) and `ETH_RESET_ERR_SERDES_CFG_LOAD` (12).
- A speed override of `0` is now applied rather than skipped, and an unsupported speed is warned about instead of being dropped silently. Firmware tables written before this release do not carry the presence bit and keep their previous trained speed.

### Persistent SPI Flash Parameters
- Expose `eth_property_table.eth_speed_override` to `bh-mod`, valid speeds - `{0, 40, 100, 200, 330, 350, 370, 400}`. `0` is interpreted as a request to auto-train.
- Expose `dram_table.soft_harvest_dram_mask` to `bh-mod`, so GDDR channels can be soft-harvested at runtime without reflashing the board config.
- Expose `feature_enable.gddr_therm_trip_en` to `bh-mod`.

### Host Interface
- The message queue ABI is now explicitly `uint32_t` rather than `uintptr_t`, so the contract no longer depends on the pointer width of the platform.
- `Dm2CmReady` moved into init so emulated boards get far enough to report the DMC firmware version.

### Drivers
- Add PLDM over MCTP support: base commands, platform commands (repository info, PDR and sensor readings), and a Tenstorrent OEM extension that exposes the Zephyr shell.
- ARC HS DMA now supports 16 channels with 256 descriptors in interrupt mode, up from 4 channels.
- Writing firmware post codes to the status scratch register is now gated on `CONFIG_TT_POST_CODE` instead of a board check, so other platforms can opt in. It defaults on for `tt_blackhole`.

### Boards
- New board: Galaxy 2 DMC (`tt_blackhole_glx2_dmc`), with GALAXY_2 SPI ROM data tables, a UART5 console on PC12/PD2, a cascaded second SPI mux (`spi_mux1`) for BH / DMC / B2B flash routing, and a MAX7221 display on SPI3.
- `bh_chip` gained an optional `spi_mux1`, which is configured by the DMC when the board provides the node.
- Documented the Blackhole DMC SPI flash mux polarity: low connects the flash to BH, high to the DMC.

### Tooling

- Add Galaxy recovery scripting that programs a hex file to ASICs which do not enumerate or answer an ARC message, then fixes up ASIC locations from PCIe bus numbers. This requires a BMC firmware build that accepts flash read, write and erase over IPMI.
- Add `smc_spi_flash.py` to flash a firmware bundle over PCIe using SMC messages, plus a helper that checks card enumeration and ARC and DMC ping responsiveness.
- Add a script that builds the manufacturing test artifacts locally instead of downloading them from CI, and a manufacturing test CI job covering the full flash-and-boot sequence on Blackhole PCIe cards.
- Consolidate the pyocd argument parsing used by `set-p300-jtag.py` and `recover-blackhole.py` into a shared helper.
- Fix two `dmc_reset.py` bugs: the PCIe enumeration stage tested the constant timeout instead of the remaining time, and the ping stage could skip a healthy DMC as "too old" when telemetry came up before the version handshake completed.

### Documentation

- Document the SMBus command registers and message payloads in `tt_smbus_regs.h` with Doxygen, including a per-command summary table and field-level breakdowns. Corrects the `CMFW_SMBUS_DM_STATIC_INFO` payload size from 160 to 192 bits.
- Update the testing documentation and add instructions for running the manufacturing test locally.

## Grendel

- Restore the correct CPU hart mapping in the Grendel SMC device tree, which had been dropped by mistake.
- MK bring-up DMC board (`tt_grendel_mk_bu_dmc`):
  - Fix four bring-up blockers that kept the board from reaching a console: the unpopulated 32.768 kHz LSE crystal is disabled, `msi-pll-mode` is dropped, chip select index 1 is corrected from PC9 (the SPI level-shifter enable) to PC7, and the unpowered `mt35xu512` flash is disabled.
  - Update the board to the STM32U375 part actually fitted, replacing the U385 used for pinctrl testing.
  - Add a POST-code display on the MAX7221 and a `post` shell command to `dm_test_app`, with `test`, `walk` and `map` sub-commands for bring-up.

## Migration guide

An overview of required and recommended changes to make when migrating from the previous v19.13.0 release can be found in [19.14 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.14.md).

## Full ChangeLog

The full ChangeLog from the previous v19.13.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.13.0...v19.14.0
