# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Cross-build wiring for libraries whose implementation has moved from C to
# Zig. A migrated library keeps its public inc/ header and drops its primary
# C implementation; support C sources can remain in src/. Without the step
# below the link would simply lose the Zig implementation, so
# every migrated library named in LIBS is built here for the app's own ARM
# target and linked as a static archive.
#
# The Zig build is driven by the library's build.zig through the same pinned
# zig binary the host tests use (tools/zig via PATH, checked by
# scripts/checks/check_tool_versions.py). Target and CPU are derived from the
# toolchain file's own flags, so the archive matches the C objects it links
# beside: same core, same float ABI, same instruction set.

find_program(RA8_ZIG_EXECUTABLE zig)

# Translate the toolchain's -mcpu into the name zig's -Dcpu expects
# (cortex-m85 -> cortex_m85). Float ABI picks the eabi/eabihf suffix.
function(_ra8_zig_target_for_toolchain _out_target _out_cpu)
  set(_cpu "")
  set(_hard_float OFF)
  foreach(_flag ${CMAKE_C_FLAGS})
    if(_flag MATCHES "^-mcpu=(.+)$")
      set(_cpu "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  string(REGEX MATCH "-mcpu=([a-z0-9._-]+)" _m "${CMAKE_C_FLAGS}")
  if(CMAKE_MATCH_1)
    set(_cpu "${CMAKE_MATCH_1}")
  endif()
  if(CMAKE_C_FLAGS MATCHES "-mfloat-abi=hard")
    set(_hard_float ON)
  endif()
  if(NOT _cpu)
    message(
      FATAL_ERROR
      "ra8: cannot derive a zig -Dcpu value: no -mcpu in CMAKE_C_FLAGS "
      "('${CMAKE_C_FLAGS}'). A migrated Zig library cannot be cross-built "
      "for an unknown core."
    )
  endif()
  string(REPLACE "-" "_" _zig_cpu "${_cpu}")
  if(_hard_float)
    set(${_out_target} "thumb-freestanding-eabihf" PARENT_SCOPE)
  else()
    set(${_out_target} "thumb-freestanding-eabi" PARENT_SCOPE)
  endif()
  set(${_out_cpu} "${_zig_cpu}" PARENT_SCOPE)
endfunction()

# Build one migrated library for this app's target and return the archive path.
function(_ra8_app_zig_library _lib _lib_path _out_archive _out_stamp)
  if(NOT RA8_ZIG_EXECUTABLE)
    message(
      FATAL_ERROR
      "ra8: '${_lib}' is a Zig library (libs/${_lib}/build.zig) but no zig "
      "binary was found on PATH. Install the pinned toolchain "
      "(just setup) before configuring an app that links it."
    )
  endif()
  _ra8_zig_target_for_toolchain(_zig_target _zig_cpu)

  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_zig_optimize Debug)
  else()
    set(_zig_optimize ReleaseSmall)
  endif()

  set(_prefix "${CMAKE_CURRENT_BINARY_DIR}/zig/${_lib}/${_zig_cpu}")
  set(_archive "${_prefix}/lib/lib${_lib}.a")

  file(GLOB_RECURSE _zig_srcs CONFIGURE_DEPENDS
       ${_lib_path}/src/*.zig ${_lib_path}/build.zig)

  add_custom_command(
    OUTPUT ${_archive}
    COMMAND
      ${CMAKE_COMMAND} -E env ${RA8_ZIG_EXECUTABLE} build
      --build-file ${_lib_path}/build.zig
      --prefix ${_prefix}
      --cache-dir ${CMAKE_CURRENT_BINARY_DIR}/zig/.cache
      -Dtarget=${_zig_target}
      -Dcpu=${_zig_cpu}
      -Doptimize=${_zig_optimize}
    DEPENDS ${_zig_srcs}
    COMMENT
      "Building Zig library ${_lib} for ${_zig_target} ${_zig_cpu} (${_zig_optimize})"
    VERBATIM
  )

  set(${_out_archive} "${_archive}" PARENT_SCOPE)
  set(${_out_stamp} "${_archive}" PARENT_SCOPE)
endfunction()

# Link every migrated library collected by sources.cmake into ${_target}.
function(_ra8_app_link_zig_libraries _target)
  if(NOT _ra8_lib_zig)
    return()
  endif()
  foreach(_entry ${_ra8_lib_zig})
    string(REPLACE "|" ";" _pair "${_entry}")
    list(GET _pair 0 _lib)
    list(GET _pair 1 _lib_path)
    _ra8_app_zig_library(${_lib} ${_lib_path} _archive _stamp)
    add_custom_target(${_target}_zig_${_lib} DEPENDS ${_stamp})
    add_dependencies(${_target} ${_target}_zig_${_lib})
    target_link_libraries(${_target} PRIVATE ${_archive})
  endforeach()
endfunction()
