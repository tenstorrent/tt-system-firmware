# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

# Extract the Sival SDK tarball once into a cache shared by every build, instead of once per build
# directory (the tree is ~100 MB, which adds up quickly across a twister run).
#
# Usage: tt_sival_sdk_extract(<tarball>)   sets SIVAL_EXTDIR to the extracted directory.
#
# The cache lives in $TT_SIVAL_SDK_CACHE, else ~/.cache/tt-sival-sdk, else the build directory.
# It is keyed by the tarball hash, so a new SDK drop gets its own directory. Extraction goes to a
# temporary directory that is moved into place, so parallel CMake runs never see a partial tree.

include_guard(GLOBAL)

macro(tt_sival_sdk_extract tarball)
  if(NOT "$ENV{TT_SIVAL_SDK_CACHE}" STREQUAL "")
    set(_sival_cache "$ENV{TT_SIVAL_SDK_CACHE}")
  elseif(NOT "$ENV{HOME}" STREQUAL "")
    set(_sival_cache "$ENV{HOME}/.cache/tt-sival-sdk")
  else()
    set(_sival_cache "${CMAKE_BINARY_DIR}/sival-sdk-cache")
  endif()

  file(SHA256 "${tarball}" _sival_hash)
  string(SUBSTRING "${_sival_hash}" 0 16 _sival_hash)
  set(SIVAL_EXTDIR "${_sival_cache}/${_sival_hash}")

  if(NOT EXISTS "${SIVAL_EXTDIR}/grendel_sival_sdk")
    string(RANDOM LENGTH 8 _sival_rand)
    set(_sival_tmp "${SIVAL_EXTDIR}.tmp.${_sival_rand}")
    file(MAKE_DIRECTORY "${_sival_tmp}")
    execute_process(COMMAND tar -xzf "${tarball}"
      WORKING_DIRECTORY "${_sival_tmp}"
      RESULT_VARIABLE _sival_rc
    )
    if(NOT _sival_rc EQUAL 0)
      file(REMOVE_RECURSE "${_sival_tmp}")
      message(FATAL_ERROR "Failed to extract ${tarball}")
    endif()
    # The rename fails harmlessly if another build moved its copy into place first.
    execute_process(COMMAND ${CMAKE_COMMAND} -E rename "${_sival_tmp}" "${SIVAL_EXTDIR}"
      RESULT_VARIABLE _sival_rc
      ERROR_VARIABLE _sival_err
      OUTPUT_QUIET
    )
    file(REMOVE_RECURSE "${_sival_tmp}")
    if(NOT EXISTS "${SIVAL_EXTDIR}/grendel_sival_sdk")
      message(FATAL_ERROR
        "Failed to install Sival SDK into cache ${SIVAL_EXTDIR} (rename rc=${_sival_rc}): "
        "${_sival_err}\nSet TT_SIVAL_SDK_CACHE to a writable directory.")
    endif()
  endif()
endmacro()
