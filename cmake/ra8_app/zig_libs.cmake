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
# beside. EIL alone lowers Zig instruction selection to the Cortex-M33 model
# used by ra8_emulator; the float ABI and normal hardware target stay unchanged.

find_program(RA8_ZIG_EXECUTABLE zig)

# Zig's optimize mode for the ported libraries, and why it is not simply
# CMAKE_BUILD_TYPE.
#
# -Doptimize=Debug builds compiler_rt at Debug too, and zig emits compiler_rt
# as ONE archive member, so a link that needs any builtin takes all of it.
# Measured on ereader_shelf for cortex_m85, allocatable bytes only:
#
#   compiler_rt   Debug 169348   ReleaseSafe ~120K   ReleaseSmall 91658
#   ra8_box own     4654                                       1075
#
# That is 77690 bytes of flash bought by nothing the linker can drop, on a
# part with 1 MB of MRAM. ereader_shelf overflowed by 127236 bytes at Debug
# and links with 1046110 bytes at ReleaseSmall. The C side never had
# this cliff because C debug codegen costs a fraction of it and the linker
# can drop unreferenced objects.
#
# So ReleaseSmall is the default for every build type, and a caller who wants
# Zig-level debug info asks for it explicitly and accepts the size:
#
#   cmake -DRA8_ZIG_OPTIMIZE=Debug ...
#
# Safety checks are NOT lost by default: ReleaseSmall keeps zig's panic
# handlers, it drops the debug-mode codegen around them.
set(RA8_ZIG_OPTIMIZE
    "ReleaseSmall"
    CACHE STRING "zig -Doptimize mode for the ported libraries"
)
set_property(
  CACHE RA8_ZIG_OPTIMIZE
  PROPERTY STRINGS
           "Debug"
           "ReleaseSafe"
           "ReleaseSmall"
           "ReleaseFast"
)

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
  # Unicorn models the M85 board as M33 and cannot execute Zig's M85 cset/csel.
  # Keep this override inside the EIL build command, never in a hardware build.
  if("$ENV{RA8_ZIG_EIL_M33}" STREQUAL "1" AND _cpu STREQUAL "cortex-m85")
    set(_cpu "cortex-m33")
  endif()
  string(REPLACE "-" "_" _zig_cpu "${_cpu}")
  if(_hard_float)
    set(${_out_target}
        "thumb-freestanding-eabihf"
        PARENT_SCOPE
    )
  else()
    set(${_out_target}
        "thumb-freestanding-eabi"
        PARENT_SCOPE
    )
  endif()
  set(${_out_cpu}
      "${_zig_cpu}"
      PARENT_SCOPE
  )
endfunction()

# Build one migrated library for an EXPLICITLY named zig target and cpu, and
# return the archive path.
#
# Split out of _ra8_app_zig_library() because not every image in an app is
# built for the app's own core: a dual-core app's second (Cortex-M33) image is
# a separate freestanding executable with its own -mcpu, so deriving the core
# from CMAKE_C_FLAGS would hand it an M85 archive. The per-cpu prefix already
# keeps the two archives of one library apart, and the guard below keeps a
# second request for the same (library, cpu) pair from declaring a duplicate
# OUTPUT rule.
function(
  _ra8_zig_build_archive
  _lib
  _lib_path
  _zig_target
  _zig_cpu
  _out_archive
)
  if(NOT RA8_ZIG_EXECUTABLE)
    message(
      FATAL_ERROR
        "ra8: '${_lib}' is a Zig library (libs/${_lib}/build.zig) but no zig "
        "binary was found on PATH. Install the pinned toolchain "
        "(just setup) before configuring an app that links it."
    )
  endif()

  set(_zig_optimize "${RA8_ZIG_OPTIMIZE}")

  set(_prefix "${CMAKE_CURRENT_BINARY_DIR}/zig/${_lib}/${_zig_cpu}")
  set(_archive "${_prefix}/lib/lib${_lib}.a")

  # One OUTPUT rule per (library, cpu) pair, however many targets ask for it.
  # The rule lives in the binary dir of the first app that asked, so a later
  # app reuses that archive path rather than its own, which has no rule.
  get_property(_declared GLOBAL PROPERTY "ra8_zig_archive_${_lib}_${_zig_cpu}")
  if(_declared)
    set(${_out_archive}
        "${_declared}"
        PARENT_SCOPE
    )
    return()
  endif()
  set_property(GLOBAL PROPERTY "ra8_zig_archive_${_lib}_${_zig_cpu}" "${_archive}")

  file(
    GLOB_RECURSE
    _zig_srcs
    CONFIGURE_DEPENDS
    ${_lib_path}/src/*.zig
    ${_lib_path}/build.zig
  )

  add_custom_command(
    OUTPUT ${_archive}
    COMMAND
      ${CMAKE_COMMAND} -E env ${RA8_ZIG_EXECUTABLE} build --build-file ${_lib_path}/build.zig
      --prefix ${_prefix} --cache-dir ${CMAKE_CURRENT_BINARY_DIR}/zig/.cache -Dtarget=${_zig_target}
      -Dcpu=${_zig_cpu} -Doptimize=${_zig_optimize}
    DEPENDS ${_zig_srcs}
    COMMENT "Building Zig library ${_lib} for ${_zig_target} ${_zig_cpu} (${_zig_optimize})"
    VERBATIM
  )

  set(${_out_archive}
      "${_archive}"
      PARENT_SCOPE
  )
endfunction()

# Build one migrated library for THIS app's own target (core and float ABI
# derived from the toolchain flags) and return the archive path.
function(
  _ra8_app_zig_library
  _lib
  _lib_path
  _out_archive
  _out_stamp
)
  _ra8_zig_target_for_toolchain(_zig_target _zig_cpu)
  _ra8_zig_build_archive(
    ${_lib}
    ${_lib_path}
    ${_zig_target}
    ${_zig_cpu}
    _archive
  )
  set(${_out_archive}
      "${_archive}"
      PARENT_SCOPE
  )
  set(${_out_stamp}
      "${_archive}"
      PARENT_SCOPE
  )
endfunction()

# Build a migrated library for an explicitly named core and link it into
# ${_target}: the entry point for a secondary image whose core differs from
# the app's own, i.e. a dual-core app's Cortex-M33 CPU1 executable.
#
#   ra8_link_zig_library_for_cpu(
#     TARGET ereader_m33_cpu1.elf
#     LIB    ra8_gfx
#     CPU    cortex_m33
#     FLOAT  hard
#   )
#
# CPU is the zig -Dcpu spelling (underscores, e.g. cortex_m33), which is what
# the image's -mcpu=cortex-m33 maps onto. FLOAT is hard (the default) or soft
# and picks the eabihf / eabi suffix, matching the image's -mfloat-abi.
# Declare the build rule for a migrated library on an explicitly named core and
# hand back the archive PATH, without linking it anywhere.
#
#   ra8_zig_archive_for_cpu(_archive LIB ra8_wdt_supervisor CPU cortex_m85)
#
# ra8_link_zig_library_for_cpu() below appends the archive to the END of a
# target's link line, which is correct whenever the archive's undefined symbols
# are satisfied by objects already ahead of it. It is wrong for a library that
# calls INTO another static library: the ThreadX watchdog supervisor references
# _tx_time_get and the _txe_* entry points, so an archive appended after
# libthreadx.a leaves all seven unresolved -- ld had already passed the library
# that defines them. Only the caller knows the order it needs, so this hands
# back the path for it to position (e.g. in an image's LINK_LIBS ahead of
# threadx_ns) instead of choosing a position on its behalf.
function(ra8_zig_archive_for_cpu _out_archive)
  cmake_parse_arguments(
    ZA
    ""
    "LIB;CPU;FLOAT"
    ""
    ${ARGN}
  )
  foreach(_required LIB CPU)
    if(NOT ZA_${_required})
      message(FATAL_ERROR "ra8_zig_archive_for_cpu(): ${_required} is required")
    endif()
  endforeach()
  if(NOT ZA_FLOAT)
    set(ZA_FLOAT hard)
  endif()
  if(ZA_FLOAT STREQUAL "hard")
    set(_zig_target "thumb-freestanding-eabihf")
  elseif(ZA_FLOAT STREQUAL "soft")
    set(_zig_target "thumb-freestanding-eabi")
  else()
    message(FATAL_ERROR "ra8_zig_archive_for_cpu(): FLOAT must be hard or soft, got '${ZA_FLOAT}'")
  endif()

  set(_lib_path "${RA8_REPO_ROOT}/libs/${ZA_LIB}")
  if(NOT EXISTS "${_lib_path}/build.zig")
    message(FATAL_ERROR "ra8_zig_archive_for_cpu(): '${ZA_LIB}' has no build.zig at "
                        "${_lib_path}; it is not a migrated Zig library."
    )
  endif()

  _ra8_zig_build_archive(
    ${ZA_LIB}
    ${_lib_path}
    ${_zig_target}
    ${ZA_CPU}
    _archive
  )
  set(${_out_archive}
      "${_archive}"
      PARENT_SCOPE
  )
endfunction()

function(ra8_link_zig_library_for_cpu)
  cmake_parse_arguments(
    ZL
    ""
    "TARGET;LIB;CPU;FLOAT"
    ""
    ${ARGN}
  )
  if(NOT ZL_TARGET)
    message(FATAL_ERROR "ra8_link_zig_library_for_cpu(): TARGET is required")
  endif()
  ra8_zig_archive_for_cpu(
    _archive
    LIB
    ${ZL_LIB}
    CPU
    ${ZL_CPU}
    FLOAT
    ${ZL_FLOAT}
  )
  add_custom_target(${ZL_TARGET}_zig_${ZL_LIB}_${ZL_CPU} DEPENDS ${_archive})
  add_dependencies(${ZL_TARGET} ${ZL_TARGET}_zig_${ZL_LIB}_${ZL_CPU})
  target_link_libraries(${ZL_TARGET} PRIVATE ${_archive})
endfunction()

# Link every migrated library collected by sources.cmake into ${_target}.
function(_ra8_app_link_zig_libraries _target)
  if(NOT _ra8_lib_zig)
    return()
  endif()
  # De-duplicate here rather than at the producer. sources.cmake names a
  # migrated library from several independent blocks -- reflow, rabook_compile,
  # jof and the comic path all pull ra8_imgdec transitively -- so an app that
  # matches two of them names it twice. One archive wants one custom target, so
  # the second registration is a hard "target already exists" configure error.
  # The producer cannot own this: a dedupe there is a point in the file, and any
  # block appended below it silently escapes. This is the one point every entry
  # passes through.
  set(_entries ${_ra8_lib_zig})
  list(REMOVE_DUPLICATES _entries)
  foreach(_entry ${_entries})
    string(REPLACE "|" ";" _pair "${_entry}")
    list(GET _pair 0 _lib)
    list(GET _pair 1 _lib_path)
    _ra8_app_zig_library(${_lib} ${_lib_path} _archive _stamp)
    add_custom_target(${_target}_zig_${_lib} DEPENDS ${_stamp})
    add_dependencies(${_target} ${_target}_zig_${_lib})
    target_link_libraries(${_target} PRIVATE ${_archive})
  endforeach()
endfunction()
