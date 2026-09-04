# SPDX-License-Identifier: Apache-2.0

if(TARGET mis_mimir)
  return()
endif()

ExternalZephyrProject_Add(
  APPLICATION mis_mimir
  SOURCE_DIR ${APP_DIR}
  BOARD tt_mmk/tt_mimir/smc
  BUILD_ONLY 1
)

ExternalZephyrProject_Add(
  APPLICATION bl1_keraunos
  SOURCE_DIR ${APP_DIR}/../bl1
  BOARD tt_mmk/tt_keraunos/smc
  BUILD_ONLY 1
)

ExternalZephyrProject_Add(
  APPLICATION bl1_mimir
  SOURCE_DIR ${APP_DIR}/../bl1
  BOARD tt_mmk/tt_mimir/smc
  BUILD_ONLY 1
)

ExternalZephyrProject_Add(
  APPLICATION bl0p5_keraunos
  SOURCE_DIR ${APP_DIR}/../bl0p5
  BOARD tt_mmk/tt_keraunos/smc
  BUILD_ONLY 1
)