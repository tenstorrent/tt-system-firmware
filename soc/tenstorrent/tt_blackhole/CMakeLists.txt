# SPDX-License-Identifier: Apache-2.0

if(CONFIG_SOC_TT_BLACKHOLE_DMC)
  include(${ZEPHYR_BASE}/soc/st/stm32/stm32g0x/CMakeLists.txt)
endif()

if(CONFIG_SOC_TT_BLACKHOLE_SMC)
  if(COMPILER STREQUAL gcc)
    # GNU compiler options
    zephyr_compile_options(-mno-sdata)
    zephyr_compile_options_ifdef(CONFIG_FPU -mfpu=fpus_all)
  else()
    # MWDT compiler options
     zephyr_compile_options(-arcv2hs -core4 -Xdual_issue -Xcode_density -Hrgf_banked_regs=32 -HL
      -Xatomic -Xll64 -Xunaligned -Xdiv_rem=radix4 -Xswap -Xbitscan -Xmpy_option=qmpyh -Xshift_assist
      -Xbarrel_shifter -Xtimer0 -Xtimer1 -Xrtc -Hld_cycles=2)

    zephyr_compile_options_ifdef(CONFIG_FPU -Xfpu_mac -Xfpud_div)

    zephyr_ld_options(-Hlib=hs48_slc_full)
  endif()

  zephyr_library()
  zephyr_library_sources(soc.c)
  zephyr_library_sources_ifdef(CONFIG_PINCTRL pinctrl_bh.c)
  zephyr_library_include_directories(PRIVATE
    ${ZEPHYR_BASE}/kernel/include
    ${ARCH_DIR}/${ARCH}/include
  )
  zephyr_library_sources_ifdef(CONFIG_FPU_SHARING fpu_init.c)

  zephyr_include_directories(${ZEPHYR_BASE}/soc/snps/hsdk4xd)

  set(SOC_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/tt_blackhole_smc.ld CACHE INTERNAL "")
endif()
