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
  # RA8P1_DP_FPU=ON appends -mfpu=fpv5-d16 to the C flags. Zig's cortex_m85
  # model is single-precision (f64 lowers to __aeabi_d* calls), so without the
  # double-precision feature every Zig object in a DP image would stay
  # soft-float for double while the C beside it runs .f64 opcodes. The pattern
  # does not match the default fpv5-sp-d16.
  if(CMAKE_C_FLAGS MATCHES "-mfpu=fpv5-d16" AND _cpu STREQUAL "cortex-m85")
    string(APPEND _zig_cpu "+fp_armv8d16")
  endif()
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

# An option-setting value arrives as the app's C define right-hand side,
# typically "0xFFFFFFF0U". The Zig unit's parser takes "0xFFFFFFF0" but not
# the C integer suffix, so strip the suffix and hand back both the parseable
# value and a path-safe archive-variant tag. Refuse anything else rather than
# let two different values collapse onto one variant name.
function(_ra8_zig_ofs_value _raw _out_value _out_tag)
  set(_v "${_raw}")
  string(REGEX REPLACE "[uUlL]+$" "" _v "${_v}")
  if(NOT _v MATCHES "^0[xX][0-9a-fA-F]+$" AND NOT _v MATCHES "^[0-9]+$")
    message(
      FATAL_ERROR
        "ra8: option-setting override value '${_raw}' is not a hex or decimal "
        "integer literal the Zig option-setting unit can take."
    )
  endif()
  string(TOLOWER "${_v}" _tag)
  string(REPLACE "0x" "x" _tag "${_tag}")
  set(${_out_value} "${_v}" PARENT_SCOPE)
  set(${_out_tag} "${_tag}" PARENT_SCOPE)
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
#
# An optional trailing OFF_TARGET builds the library with -Doff-target=true:
# the Zig spelling of the RA8_OFF_TARGET define that OFF_TARGET_LIBS gives a C
# library. An optional trailing NS_ALIAS builds it with -Dperiph_ns_alias=true
# instead (RA8FW-832): the RA8_PERIPH_NS_ALIAS spelling, so a TrustZone NS
# image's ra8_hal reaches peripherals through the IDAU bit[28]=1 alias. Each
# variant is a different archive, so it gets its own prefix and its own rule,
# and an app naming the library in LIBS still gets the real one. The two are
# exclusive: no consumer needs a library both off-target and NS-aliased.
#
# OFS1=<val> and OFS1_SEC=<val> tokens carry a per-app option-setting override
# (RA8FW-845): the app's BSP_CFG_OPTION_SETTING_OFS1(_SEC) compile definition,
# handed over so the Zig unit reads the same value the C side would. Each
# distinct value widens the variant name, so an app that overrides never
# shares one ra8_hal archive with an app at the default; two apps overriding
# to the SAME value still share one archive, which is correct because the
# words are identical.
# RA8FW-845 flip: when RA8_HAL_OFS_FROM_ZIG is on, ra8_hal's Zig unit owns the
# .option_setting_* placement (-Dofs_from_zig=true) and ra8_ofs.c must be out
# of the C set in the same configure: cmake/ra8_app/sources.cmake filters it
# from the per-app hal glob, cmake/ra8_shared_libs.cmake from the shared
# archive, and _ra8_app_zig_hal_link_one() below refuses any target that
# still compiles it. Off (default), ra8_ofs.c owns the placement and the Zig
# unit emits nothing, so the archive variant here stays un-keyed on ownership.
option(
  RA8_HAL_OFS_FROM_ZIG
  "RA8FW-845: the ra8_hal Zig unit owns the option-setting words, not ra8_ofs.c"
  OFF
)

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
        "(scripts/dev/setup_python.sh) before configuring an app that links it."
    )
  endif()

  set(_zig_optimize "${RA8_ZIG_OPTIMIZE}")

  set(_variant "${_zig_cpu}")
  set(_variant_args "")
  if("OFF_TARGET" IN_LIST ARGN)
    set(_variant "${_zig_cpu}_off_target")
    set(_variant_args "-Doff-target=true")
  endif()
  if("NS_ALIAS" IN_LIST ARGN)
    if(NOT _variant_args STREQUAL "")
      message(FATAL_ERROR "ra8: '${_lib}' was asked for as both an OFF_TARGET and an NS_ALIAS variant; the two are exclusive.")
    endif()
    set(_variant "${_zig_cpu}_ns_alias")
    set(_variant_args "-Dperiph_ns_alias=true")
  endif()

  foreach(_ofs_tok ${ARGN})
    if(_ofs_tok MATCHES "^OFS1=(.+)$")
      _ra8_zig_ofs_value("${CMAKE_MATCH_1}" _ofs_val _ofs_tag)
      string(APPEND _variant "_ofs1_${_ofs_tag}")
      list(APPEND _variant_args "-Dofs1=${_ofs_val}")
    elseif(_ofs_tok MATCHES "^OFS1_SEC=(.+)$")
      _ra8_zig_ofs_value("${CMAKE_MATCH_1}" _ofs_val _ofs_tag)
      string(APPEND _variant "_ofs1_sec_${_ofs_tag}")
      list(APPEND _variant_args "-Dofs1_sec=${_ofs_val}")
    elseif(_ofs_tok MATCHES "^OFS")
      message(
        FATAL_ERROR
          "ra8: '${_lib}' was asked for option-setting token '${_ofs_tok}'; "
          "the Zig option-setting unit carries only OFS1= and OFS1_SEC=."
      )
    endif()
  endforeach()

  # The ownership flip is part of the archive's identity: a build with the
  # Zig unit owning the words is a different archive from one where the C
  # unit does, even at the same OFS values, so the variant name widens.
  if(RA8_HAL_OFS_FROM_ZIG AND _lib STREQUAL "ra8_hal")
    string(APPEND _variant "_ofsz")
    list(APPEND _variant_args "-Dofs_from_zig=true")
  endif()

  set(_prefix "${CMAKE_CURRENT_BINARY_DIR}/zig/${_lib}/${_variant}")
  set(_archive "${_prefix}/lib/lib${_lib}.a")

  # One OUTPUT rule per (library, variant) pair, however many targets ask for
  # it. The rule lives in the binary dir of the first app that asked, so a
  # later app reuses that archive path rather than its own, which has no rule.
  get_property(_declared GLOBAL PROPERTY "ra8_zig_archive_${_lib}_${_variant}")
  if(_declared)
    set(${_out_archive}
        "${_declared}"
        PARENT_SCOPE
    )
    return()
  endif()
  set_property(GLOBAL PROPERTY "ra8_zig_archive_${_lib}_${_variant}" "${_archive}")

  # RA8FW-845: with the flip on, the words live in ra8_hal_ofs_words.o
  # inside the archive, but no code references those symbols, so the
  # member is never extracted at link time and the board linker script's
  # KEEP rules never see the sections (the C path never had this problem:
  # ra8_ofs.c is a direct app source). Extract the words object at build
  # time and hand it to the app as a direct link input below.
  if(RA8_HAL_OFS_FROM_ZIG AND _lib STREQUAL "ra8_hal")
    set(_ra8_ofs_words_obj "${_prefix}/ra8_hal_ofs_words.o")
    set(_ra8_ofs_extract_script "m=\$(${RA8_ZIG_EXECUTABLE} ar t ${_archive} | grep -F ra8_hal_ofs_words.o) && exec ${RA8_ZIG_EXECUTABLE} ar p ${_archive} \$m > ${_ra8_ofs_words_obj}")
    add_custom_command(
      OUTPUT "${_ra8_ofs_words_obj}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_prefix}"
      COMMAND /bin/bash -c "${_ra8_ofs_extract_script}"
      DEPENDS "${_archive}"
      COMMENT "Extracting ra8_hal's option-setting words object (RA8FW-845)"
      VERBATIM
    )
    set_property(GLOBAL PROPERTY "ra8_zig_words_obj_${_archive}" "${_ra8_ofs_words_obj}")
  endif()

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
      -Dcpu=${_zig_cpu} -Doptimize=${_zig_optimize} ${_variant_args}
    DEPENDS ${_zig_srcs}
    COMMENT "Building Zig library ${_lib} for ${_zig_target} ${_variant} (${_zig_optimize})"
    VERBATIM
  )

  set(${_out_archive}
      "${_archive}"
      PARENT_SCOPE
  )
endfunction()

# Build one board boot unit written in Zig (src/boot/<unit>.zig) as a
# standalone object for THIS app's core and return its path (RA8FW-616).
#
# An object, never an archive member: both board vector tables define the
# exception handlers as weak aliases of Default_Handler, and that weak
# definition is already in the link, so a handler in an archive would never
# be pulled in and the fault would land in Default_Handler. The rule is
# de-duplicated per directory, not globally like the archives above: a
# generated object is only buildable from the directory that declared it.
function(
  _ra8_app_zig_boot_object
  _board_dir
  _unit
  _cache_mpu
  _mpu_via_hal
  _cache_via_hal
  _out_object
)
  if(NOT RA8_ZIG_EXECUTABLE)
    message(
      FATAL_ERROR
        "ra8: ${_board_dir}/src/boot/${_unit} is a Zig boot unit but no zig "
        "binary was found on PATH. Install the pinned toolchain "
        "(scripts/dev/setup_python.sh) before configuring an app that links it."
    )
  endif()
  _ra8_zig_target_for_toolchain(_zig_target _zig_cpu)
  get_filename_component(_board "${_board_dir}" NAME)
  # A BOOT_PROFILE unit arrives as "<profile>/<stem>.zig"; keep the profile
  # in the key so it never shares an object with the board default.
  string(REGEX REPLACE "\\.zig$" "" _stem "${_unit}")
  string(REPLACE "/" "_" _stem "${_stem}")
  # The unit reads `@import("boot_options")`, so the object is also keyed on
  # what that module says: one build per TrustZone setting (RA8FW-622) and per
  # cache/MPU gate spelling (RA8FW-365). The Zig graph writes the same fields
  # from cross_sources.BootOptions. The gates arrive as the RA8_APP_BOOT_*
  # directory variables the app's CMakeLists set before ra8_add_app().
  if(RA8_TRUSTZONE_ENABLE)
    set(_tz tz)
    set(_tz_zig true)
  else()
    set(_tz notz)
    set(_tz_zig false)
  endif()
  # "ON"/"OFF" (or empty for unset) become the Zig spellings. An unset gate
  # is off, matching the C tree's default of no define.
  if(_cache_mpu)
    set(_cm true)
  else()
    set(_cm false)
  endif()
  if(_mpu_via_hal)
    set(_mh true)
  else()
    set(_mh false)
  endif()
  if(_cache_via_hal)
    set(_ch true)
  else()
    set(_ch false)
  endif()
  set(_dir "${CMAKE_CURRENT_BINARY_DIR}/zig/boot/${_board}/${_zig_cpu}/${_tz}")
  set(_object "${_dir}/${_stem}.o")
  set(_options "${_dir}/boot_options.zig")
  get_property(_declared DIRECTORY PROPERTY "ra8_zig_boot_${_board}_${_stem}_${_zig_cpu}_${_tz}_cm${_cm}_mh${_mh}_ch${_ch}")
  if(NOT _declared)
    set_property(DIRECTORY PROPERTY "ra8_zig_boot_${_board}_${_stem}_${_zig_cpu}_${_tz}_cm${_cm}_mh${_mh}_ch${_ch}" ON)
    file(WRITE "${_options}" "pub const trust_zone: bool = ${_tz_zig};\n"
    "pub const cache_mpu: bool = ${_cm};\n"
    "pub const mpu_via_hal: bool = ${_mh};\n"
    "pub const cache_via_hal: bool = ${_ch};\n")
    set(_source "${_board_dir}/src/boot/${_unit}")
    set(_module_flags -target ${_zig_target} -mcpu ${_zig_cpu} -O ${RA8_ZIG_OPTIMIZE}
                      -fno-unwind-tables
    )
    add_custom_command(
      OUTPUT ${_object}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${_dir}
      COMMAND
        ${CMAKE_COMMAND} -E env ${RA8_ZIG_EXECUTABLE} build-obj ${_module_flags} --dep
        boot_options -Mroot=${_source} ${_module_flags} -Mboot_options=${_options} --cache-dir
        ${CMAKE_CURRENT_BINARY_DIR}/zig/.cache -femit-bin=${_object}
      DEPENDS ${_source} ${_options}
      COMMENT "Building Zig boot unit ${_board}/${_unit} for ${_zig_target} ${_zig_cpu}"
      VERBATIM
    )
    set_source_files_properties(${_object} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  endif()
  set(${_out_object}
      "${_object}"
      PARENT_SCOPE
  )
endfunction()

# Build one migrated library for THIS app's own target (core and float ABI
# derived from the toolchain flags) and return the archive path. A trailing
# OFF_TARGET is passed through to _ra8_zig_build_archive().
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
    ${ARGN}
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
# A trailing NS_ALIAS asks for the periph_ns_alias archive variant (RA8FW-832)
# and rides through to _ra8_zig_build_archive() like an OFF_TARGET would.
function(ra8_zig_archive_for_cpu _out_archive)
  cmake_parse_arguments(
    ZA
    "NS_ALIAS"
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

  set(_za_flags)
  if(ZA_NS_ALIAS)
    list(APPEND _za_flags NS_ALIAS)
  endif()
  _ra8_zig_build_archive(
    ${ZA_LIB}
    ${_lib_path}
    ${_zig_target}
    ${ZA_CPU}
    _archive
    ${_za_flags}
  )
  set(${_out_archive}
      "${_archive}"
      PARENT_SCOPE
  )
endfunction()

# A trailing NS_ALIAS links the periph_ns_alias archive variant (RA8FW-832).
# The custom target's name carries the variant suffix, so an image that links
# the same library on the same CPU both ways cannot collide on one target.
function(ra8_link_zig_library_for_cpu)
  cmake_parse_arguments(
    ZL
    "NS_ALIAS"
    "TARGET;LIB;CPU;FLOAT"
    ""
    ${ARGN}
  )
  if(NOT ZL_TARGET)
    message(FATAL_ERROR "ra8_link_zig_library_for_cpu(): TARGET is required")
  endif()
  set(_zl_flags)
  if(ZL_NS_ALIAS)
    list(APPEND _zl_flags NS_ALIAS)
  endif()
  ra8_zig_archive_for_cpu(
    _archive
    LIB
    ${ZL_LIB}
    CPU
    ${ZL_CPU}
    FLOAT
    ${ZL_FLOAT}
    ${_zl_flags}
  )
  set(_variant_suffix "")
  if(ZL_NS_ALIAS)
    set(_variant_suffix "_ns_alias")
  endif()
  add_custom_target(${ZL_TARGET}_zig_${ZL_LIB}_${ZL_CPU}${_variant_suffix} DEPENDS ${_archive})
  add_dependencies(${ZL_TARGET} ${ZL_TARGET}_zig_${ZL_LIB}_${ZL_CPU}${_variant_suffix})
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
  set(_ra8_hal_archive "")
  foreach(_entry ${_entries})
    string(REPLACE "|" ";" _pair "${_entry}")
    list(GET _pair 0 _lib)
    list(GET _pair 1 _lib_path)
    # An OFF_TARGET_LIBS entry carries a third field, "off_target".
    set(_variant "")
    set(_suffix "")
    list(LENGTH _pair _fields)
    if(_fields GREATER 2)
      set(_variant OFF_TARGET)
      set(_suffix "_off_target")
    endif()
    _ra8_app_zig_library(
      ${_lib}
      ${_lib_path}
      _archive
      _stamp
      ${_variant}
    )
    if(_lib STREQUAL "ra8_hal")
      # RA8FW-845: the option-setting words' archive variant is keyed on the
      # app's own BSP_CFG_OPTION_SETTING_OFS1(_SEC) compile definitions, and
      # those may be set only AFTER ra8_add_app() returns -- this function
      # runs inside it. So the HAL link is deferred to the end of the app's
      # directory, where the definitions are final. The deferred link still
      # lands after app-selected Zig libraries, which preserves the ordering
      # the old end-of-list HAL link guaranteed: the HAL archive provides
      # cache maintenance used by the display PAL, so GNU ld must see the
      # other libraries' undefined references before scanning libra8_hal.a.
      # The deferred call's arguments are re-expanded at execution time, in
      # a scope where this function's locals are gone, so the values ride in
      # a DIRECTORY property instead of DEFER arguments.
      set_property(
        DIRECTORY APPEND PROPERTY "ra8_hal_defer_links" "${_target}|${_lib_path}"
      )
      cmake_language(DEFER DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} CALL _ra8_app_zig_hal_link)
      continue()
    endif()
    add_custom_target(${_target}_zig_${_lib}${_suffix} DEPENDS ${_stamp})
    add_dependencies(${_target} ${_target}_zig_${_lib}${_suffix})
    target_link_libraries(${_target} PRIVATE ${_archive})
  endforeach()
endfunction()

# The deferred half of the ra8_hal link (RA8FW-845). Runs at the end of the
# app's directory, after the app had its last word on
# target_compile_definitions. Reads BSP_CFG_OPTION_SETTING_OFS1(_SEC) off the
# image and keys the option-setting archive variant on them, so an app that
# overrides never shares the default archive. Any other
# BSP_CFG_OPTION_SETTING_* define is refused: the Zig unit does not take it
# yet, and linking the default archive would silently drop the override.
function(_ra8_app_zig_hal_link)
  get_property(_hal_links DIRECTORY PROPERTY "ra8_hal_defer_links")
  foreach(_link ${_hal_links})
    string(REPLACE "|" ";" _hal_pair "${_link}")
    list(GET _hal_pair 0 _target)
    list(GET _hal_pair 1 _lib_path)
    _ra8_app_zig_hal_link_one(${_target} "${_lib_path}")
  endforeach()
endfunction()

function(_ra8_app_zig_hal_link_one _target _lib_path)
  set(_tokens)
  get_target_property(_defs ${_target} COMPILE_DEFINITIONS)
  if(NOT _defs)
    set(_defs "")
  endif()
  foreach(_def ${_defs})
    if(_def MATCHES "^-?D?BSP_CFG_OPTION_SETTING_OFS1_SEC=(.+)$")
      list(APPEND _tokens "OFS1_SEC=${CMAKE_MATCH_1}")
    elseif(_def MATCHES "^-?D?BSP_CFG_OPTION_SETTING_OFS1=(.+)$")
      list(APPEND _tokens "OFS1=${CMAKE_MATCH_1}")
    elseif(_def MATCHES "^-?D?BSP_CFG_OPTION_SETTING_")
      message(
        FATAL_ERROR
          "ra8: ${_target} defines '${_def}' but the Zig option-setting unit "
          "carries only OFS1/OFS1_SEC overrides (RA8FW-845); linking it onto "
          "the default archive would silently drop the override."
      )
    endif()
  endforeach()
  if(RA8_HAL_OFS_FROM_ZIG)
    # Double-ownership gate (RA8FW-845): with the Zig unit owning the
    # .option_setting_* placement, a target that still compiles ra8_ofs.c
    # would put both objects on the same pinned MRAM output sections. The
    # normal hal glob is already filtered; this catches any other path that
    # reintroduces the TU into this target.
    get_target_property(_ra8_hal_srcs ${_target} SOURCES)
    if(NOT _ra8_hal_srcs)
      set(_ra8_hal_srcs "")
    endif()
    list(FILTER _ra8_hal_srcs INCLUDE REGEX "/ra8_ofs\\.c$")
    if(_ra8_hal_srcs)
      message(
        FATAL_ERROR
          "ra8: RA8_HAL_OFS_FROM_ZIG is on but ${_target} still compiles "
          "libs/ra8_hal/src/ra8_ofs.c; both units would own the "
          ".option_setting_* placement (RA8FW-845). The per-app hal glob is "
          "supposed to drop it (cmake/ra8_app/sources.cmake)."
      )
    endif()
  endif()

  _ra8_app_zig_library(ra8_hal "${_lib_path}" _archive _stamp ${_tokens})
  add_custom_target(${_target}_zig_ra8_hal DEPENDS ${_stamp})
  add_dependencies(${_target} ${_target}_zig_ra8_hal)
  target_link_libraries(${_target} PRIVATE ${_archive})
  # RA8FW-845: the words object has no referenced symbols, so it must reach
  # the link as a direct input, not as an archive member the resolver would
  # never pull in. The board linker script's KEEP rules then retain the
  # .option_setting_* sections exactly as they retain the C unit's.
  get_property(_ra8_ofs_words_obj GLOBAL PROPERTY "ra8_zig_words_obj_${_archive}")
  if(_ra8_ofs_words_obj)
    set_source_files_properties("${_ra8_ofs_words_obj}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    target_sources(${_target} PRIVATE "${_ra8_ofs_words_obj}")
  endif()
endfunction()
