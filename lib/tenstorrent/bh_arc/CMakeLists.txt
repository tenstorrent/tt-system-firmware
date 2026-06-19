# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(bh_arc)

zephyr_library_include_directories_ifdef(CONFIG_TT_BH_ARC ${ZEPHYR_CURRENT_MODULE_DIR}/include)

zephyr_library_sources_ifndef(
  CONFIG_TT_SMC_RECOVERY
# zephyr-keep-sorted-start
  aiclk_ppm.c
  bh_power.c
  clock_wave.c
  counter.c
  dvfs.c
  efuse.c
  eth.c
  fan_ctrl.c
  functional_efuse.c
  gddr.c
  harvesting.c
  i2c_messages.c
  led.c
  noc.c
  noc_init.c
  pcie_dma.c
  pcie_msi.c
  pvt.c
  regulator.c
  regulator_config.c
  serdes_eth.c
  telemetry.c
  telemetry_internal.c
  tensix.c
  tensix_init.c
  throttler.c
  vf_curve.c
  voltage.c
# zephyr-keep-sorted-stop
)

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
zephyr_library_sources_ifdef(CONFIG_TT_BH_ARC_EMUL reg_stub.c)

zephyr_library_add_dependencies(nanopb_generated_headers)

zephyr_library_sources_ifdef(CONFIG_TT_SHELL tt_shell.c)

zephyr_linker_sources(DATA_SECTIONS iterables.ld)
if(CONFIG_ARC)
  zephyr_library_link_libraries(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/blobs/tt_blackhole_libpciesd.a)
endif()
