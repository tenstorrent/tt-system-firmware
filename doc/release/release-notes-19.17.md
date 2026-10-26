# v19.17.0

We are pleased to announce the release of TT System Firmware version 19.17.0 🥳🎉.

## What's Changed

## Blackhole

### Ethernet

- Updated Blackhole ERISC FW to v1.13.1, change list from v1.13.0:
  - UBB retimer ports now train at 330G instead of 200G when auto-train is enabled (i.e. set speed=0). This is not enabled by default.
  - Reduced the default DFE ratio for UBB QSFP-DD ports to remove uncorrected errors.
  - Increased the instruction cache flush before handing the ERISC core to tt-metal, to avoid running stale instructions.
  - SerDes register reads now update the shared NOC read counters, so tt-metal's NOC barriers stay in sync.
  - Added an execution-stage breadcrumb (`eth_status.eth_fw_stage`), shared with tt-metal, showing what the Ethernet firmware is currently doing.

### Persistent SPI Flash Parameters

- Add `feature_enable.bypass_cable_fault_detection` to the firmware table and
  expose it to `bh-mod`. When set, the SMC skips the boot-time cable fault
  check (the 12V-2x6 power limit the DMC reports), so a 0 W limit no longer
  puts the chip into low-power cable fault mode. Off in every board table;
  intended for bring-up and debug. `bh-mod res` restores the cmfwcfg value.
- Expose `chip_limits.max_tdp_limit` to `bh-mod` (`CCFGOVR`), allowing the maximum TDP limit to be overridden independently of `chip_limits.tdp_limit`.

### SPI Flash

- `flash_mux` now reads the JEDEC ID once using the single-SPI fallback device and selectively initializes only the matching candidate EEPROM driver (via `zephyr,deferred-init`), eliminating false JEDEC ID mismatch errors logged at boot when probing non-matching candidates.
- Add a `winbond,w25q51rw` MSPI NOR flash quirk (`winbond,output-driver-ohms`) that programs Status Register-3 `DRV1:DRV0` at initialization, and set the output driver strength to 25 ohms (`<25>`) on Blackhole to resolve quad-SPI read signal integrity issues on IO0.
- Move `compatible` out of `TT_SPI_FLASH_COMMON` into each flash candidate node so chip-specific bindings can be used, and read bootrom SPI handoff parameters from the `flash_single_io` fallback node.

### DMC & Boards

- Galaxy 2 DMC (`tt_blackhole_glx2_dmc`):
  - Default to the maximum supported PSU power limit (600 W) when `psu_sense0` / `psu_sense1` GPIOs are absent instead of dereferencing a null GPIO port.
  - Disable the unpopulated LSE crystal, enable LSI, and remove `msi-pll-mode`.
  - Enable the Zephyr shell over SEGGER RTT (`CONFIG_SHELL_BACKEND_RTT`) in the DMC application and add a `tt_blackhole_glx2_dmc` board configuration for `dm_test_app`.
- `nucleo_u385rg_q`: enable I3C target, MCTP over I3C, and PLDM responder/shell Kconfig options for DMC builds, and set the I3C DCR to `0xcc` (MCTP).
- STM32 I3C & MCTP over I3C (via `zephyr-fork`):
  - Add target-mode In-Band Interrupt (`i3c_target_ibi_raise`) support to the STM32 I3C driver and remove the blocking `ibi_lock_sem` take from the controller IBI ISR.
  - Add Packet Error Code (PEC) support for MCTP over I3C bindings (both buffer-mode and byte-by-byte targets, with receive processing deferred to a work item).
  - Add MCTP over I3C controller polling mode (`CONFIG_MCTP_I3C_CONTROLLER_POLLING_MODE`) with `I3C_MSG_NOACK_EXPECTED` support for targets without IBI.

### Tooling

- Add Blackhole recovery and `tt-bootstrap` flash-loader (`bh_flm_u3`) support for STM32U3-based DMC boards (`stm32u375veix`, including `galaxy_2`).

## Grendel

### Boards & SoC

- New board: `tt_qsr1a1` for the QSR1.A1 package, supporting 1 Keraunos IO chiplet (`tt_qsr1a1/tt_keraunos/smc`), 4 Mimir memory chiplets (`tt_qsr1a1/tt_mimir/smc`), and 1 Quasar chiplet (`tt_qsr1a1/tt_quasar/smc`), along with the new `tt_quasar` SoC definition.
- Set `CONFIG_PMP_GRANULARITY=4096` on `tt_grendel_smc` to avoid excessive power-of-two PMP alignment padding in Grendel SMC images.

### Boot & Libraries

- Centralize the BL1/BL0p5 image jump sequence in `gr_smc` and disable all machine interrupt sources before jumping to prevent spurious interrupts in the target image.
- Increase BL1 stack sizes to support mailbox and I3C interrupt handling.

### Drivers

- Add Grendel I3C controller (`tenstorrent,grendel-i3c-controller`) and target (`tenstorrent,grendel-i3c-target`) drivers to manage communication between orchestrator and target chiplets prior to D2D bring-up.
- `tt_smc_remoteproc`: always issue I3C `ENTDAA` even when no `boot-gpios` synchronization pin is configured.

## Tests & CI

- Fix `tests/drivers/tt_pcie` by staging a fake firmware bundle with real SerDes firmware and allowing the test to complete without halting.
- Add the `metal-deploy-test` CI workflow and bump `metal-test` images to `v0.78.0`.

## Migration guide

An overview of required and recommended changes to make when migrating from the previous v19.16.0 release can be found in [19.17 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.17.md).

## Full ChangeLog

The full ChangeLog from the previous v19.16.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.16.0...v19.17.0
