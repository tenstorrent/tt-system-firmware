# SPDX-License-Identifier: Apache-2.0

# bh_arc is split into two Zephyr libraries:
#   - bh_arc_recovery: shared + recovery code, always built.
#   - bh_arc_mission: mission-only sources, built only for non-recovery images.
#     It is layered on top of bh_arc_recovery.

zephyr_library_named(bh_arc_recovery)
zephyr_library_include_directories_ifdef(CONFIG_TT_BH_ARC ${ZEPHYR_CURRENT_MODULE_DIR}/include)

zephyr_library_sources(
# zephyr-keep-sorted-start
  asic_state.c
  avs.c
  cat.c
  cm2dm_msg.c
  dw_apb_i2c.c
  harvesting.c
  led.c
  log.c
  noc.c
  noc2axi.c
  noc_init.c
  pcie.c
  pll.c
  post_code.c
  reset.c
  smbus_target.c
  spi_eeprom.c
  spi_flash_buf.c
  telemetry.c
  timer.c
# zephyr-keep-sorted-stop
)

zephyr_library_sources_ifdef(CONFIG_TT_BH_ARC_MSGQUEUE msgqueue.c)
zephyr_library_sources_ifdef(CONFIG_TT_BH_ARC_RUNTIME_TELEMETRY runtime_telemetry.c)
zephyr_library_sources_ifdef(CONFIG_TT_BH_ARC_EMUL reg_stub.c)

# Backend for the chip-info service: the static (no-SPI) one when fwtable is
# unavailable (recovery), the fwtable-backed one (bh_arc_mission) otherwise.
zephyr_library_sources_ifndef(CONFIG_BH_FWTABLE chip_info_static.c)

# harvesting.c and telemetry.c include bh_fwtable.h (and thus the generated
# nanopb headers) behind CONFIG_BH_FWTABLE guards, so when fwtable is enabled
# this library needs the generated headers to exist before it is compiled.
if(CONFIG_BH_FWTABLE)
  zephyr_library_add_dependencies(nanopb_generated_headers)
endif()

zephyr_linker_sources(DATA_SECTIONS iterables.ld)
if(CONFIG_ARC)
  zephyr_library_link_libraries(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/blobs/tt_blackhole_libpciesd.a)
endif()

if(NOT CONFIG_TT_SMC_RECOVERY)
  zephyr_library_named(bh_arc_mission)
  zephyr_library_include_directories_ifdef(CONFIG_TT_BH_ARC ${ZEPHYR_CURRENT_MODULE_DIR}/include)

  zephyr_library_sources(
    aiclk_ppm.c
    bh_power.c
    clock_wave.c
    counter.c
    dvfs.c
    eth.c
    fan_ctrl.c
    functional_efuse.c
    gddr.c
    i2c_messages.c
    pcie_dma.c
    pcie_msi.c
    pvt.c
    regulator.c
    regulator_config.c
    serdes_eth.c
    telemetry_internal.c
    tensix.c
    tensix_init.c
    throttler.c
    vf_curve.c
    voltage.c
  )

  zephyr_library_sources_ifdef(CONFIG_BH_FWTABLE chip_info_fwtable.c)
  zephyr_library_sources_ifdef(CONFIG_TT_SHELL tt_shell.c)
  zephyr_library_add_dependencies(nanopb_generated_headers)
endif()
