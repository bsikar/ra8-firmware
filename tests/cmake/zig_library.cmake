# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Host-build consumption of a first-party library whose implementation has
# moved from C to Zig. The library keeps its hand-authored C23 public header
# under libs/<lib>/inc/, so every existing C test, example and consumer is
# unchanged: only the object code behind the ABI is now produced by
# `zig build` instead of by the host C compiler.
#
# This is deliberately narrower than tests/cmake/zig_abi_contract.cmake, which
# owns the ABI *fixture* (C + Rust consumers + negative compile probes). Here
# the proof is the existing C unit-test suite: it links the Zig static library
# and must keep passing unchanged.

include(CMakeParseArguments)

# Build one migrated library's Zig static archive and expose it as an IMPORTED
# target named ra8_zig::<NAME>.
function(ra8_add_zig_library)
  set(_ra8_one_value_args NAME ZIG_ROOT LIBRARY_NAME)
  cmake_parse_arguments(RA8_ZIG "" "${_ra8_one_value_args}" "" ${ARGN})

  foreach(_ra8_required IN LISTS _ra8_one_value_args)
    if(NOT RA8_ZIG_${_ra8_required})
      message(FATAL_ERROR "ra8_add_zig_library requires ${_ra8_required}")
    endif()
  endforeach()

  # Several host tools and the aggregate tests each declare the libraries they
  # link, so a second declaration of the same archive is a no-op, not an error.
  if(TARGET ${RA8_ZIG_NAME}_zig_library)
    return()
  endif()

  if(NOT ZIG_EXECUTABLE)
    find_program(ZIG_EXECUTABLE NAMES zig)
  endif()
  if(NOT ZIG_EXECUTABLE)
    message(FATAL_ERROR "${RA8_ZIG_NAME}: migrated Zig library requires zig on PATH")
  endif()

  set(_ra8_output_dir "${CMAKE_CURRENT_BINARY_DIR}/zig_libs/${RA8_ZIG_NAME}")
  set(_ra8_library_file
      "${CMAKE_STATIC_LIBRARY_PREFIX}${RA8_ZIG_LIBRARY_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX}"
  )
  set(_ra8_library "${_ra8_output_dir}/lib/${_ra8_library_file}")

  # The library's own sources are the command's inputs, so the edge settles
  # once built. A custom target with only BYPRODUCTS has no outputs, and
  # ninja re-runs it on every invocation, re-grinding every built library
  # each chunk. CONFIGURE_DEPENDS re-globs at build time, so a new file
  # counts too.
  file(
    GLOB_RECURSE
    _ra8_zig_sources
    CONFIGURE_DEPENDS
    "${RA8_ZIG_ZIG_ROOT}/src/*.zig"
    "${RA8_ZIG_ZIG_ROOT}/build.zig"
    "${RA8_ZIG_ZIG_ROOT}/build.zig.zon"
  )

  # One global cache for every migrated library in the tree: zig's global
  # cache is content-addressed, so sharing it de-duplicates compiler_rt and
  # std across ~60 libraries instead of burning a private copy per library.
  set(_ra8_global_cache "${CMAKE_CURRENT_BINARY_DIR}/zig-global-cache")

  add_custom_command(
    OUTPUT "${_ra8_library}"
    COMMAND
      "${CMAKE_COMMAND}" -E env "ZIG_GLOBAL_CACHE_DIR=${_ra8_global_cache}"
      "${ZIG_EXECUTABLE}" build -Doptimize=Debug --prefix "${_ra8_output_dir}" --cache-dir
      "${_ra8_output_dir}/cache"
    DEPENDS ${_ra8_zig_sources}
    WORKING_DIRECTORY "${RA8_ZIG_ZIG_ROOT}"
    COMMENT "Building migrated Zig library ${RA8_ZIG_NAME}"
    VERBATIM
  )

  add_custom_target(${RA8_ZIG_NAME}_zig_library ALL DEPENDS "${_ra8_library}")

  add_library(ra8_zig::${RA8_ZIG_NAME} STATIC IMPORTED GLOBAL)
  set_target_properties(
    ra8_zig::${RA8_ZIG_NAME} PROPERTIES IMPORTED_LOCATION "${_ra8_library}"
                                        INTERFACE_INCLUDE_DIRECTORIES "${RA8_ZIG_ZIG_ROOT}/inc"
  )
  add_dependencies(ra8_zig::${RA8_ZIG_NAME} ${RA8_ZIG_NAME}_zig_library)

  # The Zig test step is the library's own unit suite; the C suite that links
  # this archive stays the behavioural contract.
  add_test(
    NAME ${RA8_ZIG_NAME}_zig_tests
    COMMAND "${CMAKE_COMMAND}" -E env "ZIG_GLOBAL_CACHE_DIR=${_ra8_global_cache}"
            "${ZIG_EXECUTABLE}" build test --cache-dir "${_ra8_output_dir}/cache"
    WORKING_DIRECTORY "${RA8_ZIG_ZIG_ROOT}"
  )
endfunction()
