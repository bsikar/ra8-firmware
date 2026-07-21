#
# cmake/ra8_add_app.cmake -- shared per-app firmware build recipe.
#
# Each example app's CMakeLists.txt is reduced to a thin stub:
#
#     cmake_minimum_required(VERSION 3.20)
#     set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
#     while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
#         get_filename_component(_d "${_d}" DIRECTORY)
#     endwhile()
#     include("${_d}/cmake/ra8_add_app.cmake")
#     ra8_add_app(NAME blink STACK_BYTES 2200 DESCRIPTION "Bare-metal blink firmware")
#
# ra8_add_app() builds <NAME>.elf/.hex/.bin from:
#   - main.c                         : always taken from the app dir
#   - vector_table.c / system_init.c / secure_exception.c / nmi_exception.c /
#     trustzone_init.c :
#       the app's local copy if it exists (per-app override), else the
#       selected board's libs/ra8_board_<BOARD>/boot/ copy (BOARD defaults to
#       ek_ra8d2 -- see the option below). The shared vector_table.c is
#       the plurality variant (weak-alias handlers + ra8_lpm_safe_boot() early
#       hook); apps with a divergent table (ra8_isr_dispatch dispatcher, no LPM
#       hook, dual-core / TrustZone reset) keep their own copy and override it.
#   - linker_script.ld               : the app's local copy if it exists
#       (divergent maps: dual-core, TrustZone, bootloader banks), else the
#       selected board's canonical single-core map
#       libs/ra8_board_<BOARD>/ld/linker_script.ld
#   - the ra8_* libraries + src/secure_app
#
# Options:
#   NAME <n>            (required) app + elf base name
#   STACK_BYTES <n>     per-function stack-frame budget (default 2200)
#   DESCRIPTION <s>     project() description for standalone builds
#   BOARD <b>           board-support layer under libs/ra8_board_<b> (default
#                       ek_ra8d2). Selects which board layer supplies the fallback
#                       boot files, the board src glob, the fallback linker script,
#                       and the board include path. Left unset every existing app
#                       resolves to libs/ra8_board_ek_ra8d2 -- the exact hardcoded
#                       paths this recipe used before the parameter existed, so the
#                       default build is byte-for-behaviour unchanged. The RA8P1
#                       foundation apps pass BOARD ra8p1 to build against the
#                       libs/ra8_board_ra8p1 layer (issue #226).
#   NO_NSC              exclude the ra8_nsc sources (secure-only dual-core apps)
#   USES <m>...         vendored middleware to enable + link. Each <m> maps to
#                       cmake/<m>.cmake (interface lib target <m>) and, when a
#                       global property RA8_<M>_PORT_SOURCES exists, that
#                       property's bridge sources. Recognised: threadx usbx
#                       netxduo filex levelx nimble mbedtls. In standalone
#                       builds RA8_USE_<M> defaults ON; in the aggregate build it
#                       stays as the top-level set it, and an app whose
#                       middleware is OFF skips itself.
#   LIBS <l>...         extra first-party libraries under libs/<l> to glob +
#                       add to the include path.
#   SIM_LIBS <l>...     like LIBS, but the library's sources are compiled with
#                       RA8_SIMULATOR_MODE defined.
#   EXTRA_SRCS <f>...   explicit extra .c files compiled into this app (paths
#                       relative to the app dir or absolute). Each file's parent
#                       directory is added to the include path so a co-located
#                       header is found. Use this to share a helper TU across
#                       sibling apps (e.g. examples/.../common/foo.c) without a
#                       full library.
#
# Implemented as a macro so project()/set()/return() land in the caller's
# directory scope (project() may not be called from a function, and the
# middleware skip-guard must return from the app's own CMakeLists).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Captured at include time (file scope) so it points at cmake/, not the
# caller's dir -- inside a macro CMAKE_CURRENT_LIST_DIR is the caller's.
set(_RA8_ADD_APP_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Declare one cross-compiled example application.
#
# Generates the .elf/.hex/.bin targets, links the shared library set plus
# any LIBS the app names, applies the project warning profile at the app's
# STACK_BYTES budget, and wires the per-app linker script. A macro rather
# than a function so a standalone `cmake -S <app> -B build` can re-run
# project() in the caller's scope.
#
#   NAME        <app>          target basename (required)
#   STACK_BYTES <n>            -Wstack-usage budget (default 2200)
#   DESCRIPTION <text>         project() description
#   BOARD       <lib>          board support library
#   NO_NSC                     skip the TrustZone NSC veneer objects
#   USES/LIBS/SIM_LIBS         extra link libraries (firmware / board_sim)
#   NSC_SRCS/EXTRA_SRCS        extra sources
# cmake-lint: disable=R0912,R0915
#
# The branch and statement ceilings are waived for this macro alone. Its 51
# branches ARE the app matrix -- one arm per optional feature an app can ask
# for (TrustZone NSC, CPU1 image, board, sim libs, extra sources). Splitting
# it into helpers would not remove a branch, only move it somewhere the app
# author has to go and find. The waiver is per-file and deliberate; the
# global ceilings in .cmake-format.yaml stay at cmakelang defaults so no
# other listfile inherits it.
# The two largest phases of ra8_add_app() live beside this file, one per
# question they answer (#359). Included once here; both define a macro that
# ra8_add_app() calls, so the expanded result is the code that was inline.
include(${_RA8_ADD_APP_DIR}/ra8_app/sources.cmake)
include(${_RA8_ADD_APP_DIR}/ra8_app/vendored.cmake)

macro(ra8_add_app)
  cmake_parse_arguments(
    _RA8_APP
    "NO_NSC"
    "NAME;STACK_BYTES;DESCRIPTION;BOARD"
    "USES;LIBS;SIM_LIBS;NSC_SRCS;EXTRA_SRCS"
    ${ARGN}
  )

  if(NOT _RA8_APP_NAME)
    message(FATAL_ERROR "ra8_add_app(): NAME is required")
  endif()
  if(NOT _RA8_APP_STACK_BYTES)
    set(_RA8_APP_STACK_BYTES 2200)
  endif()
  if(NOT _RA8_APP_DESCRIPTION)
    set(_RA8_APP_DESCRIPTION "RA8D2 firmware: ${_RA8_APP_NAME}")
  endif()
  # Board-support layer selector (issue #226). Unset -> ek_ra8d2, so every
  # existing app resolves to the exact libs/ra8_board_ek_ra8d2 paths this recipe
  # hardcoded before the parameter existed and rebuilds byte-identically.
  if(NOT _RA8_APP_BOARD)
    set(_RA8_APP_BOARD "ek_ra8d2")
  endif()

  # Repo root = parent of the dir holding this file (cmake/).
  get_filename_component(RA8_REPO_ROOT "${_RA8_ADD_APP_DIR}/.." ABSOLUTE)

  # Absolute path to the selected board layer. All four board references below
  # (fallback boot files, board src glob, fallback linker script, board include
  # path) derive from this one variable so the board is swapped in one place.
  set(_ra8_board_dir "${RA8_REPO_ROOT}/libs/ra8_board_${_RA8_APP_BOARD}")
  if(NOT EXISTS "${_ra8_board_dir}")
    message(
      FATAL_ERROR "ra8_add_app(): BOARD '${_RA8_APP_BOARD}' has no layer at ${_ra8_board_dir}"
    )
  endif()

  # ---- standalone vs embedded -------------------------------------------
  # NOTE: the per-app stub already contains its own literal, guarded
  # project() call -- CMake requires the top-level listfile of a standalone
  # `cmake -S . -B build` to contain a direct project() command, and a call
  # made here from inside a macro does not satisfy that rule. This second
  # project() re-affirms it with the shared VERSION and the app description;
  # it is cheap (compiler detection is already cached) and warning-free. Do
  # not remove the stub's project() to "dedupe" -- the dev warning returns.
  get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
  if(NOT _ra8_has_parent)
    file(READ "${RA8_REPO_ROOT}/VERSION" _ra8_ver)
    string(STRIP "${_ra8_ver}" _ra8_ver)
    project(
      ${_RA8_APP_NAME}
      LANGUAGES C ASM
      VERSION ${_ra8_ver}
      DESCRIPTION "${_RA8_APP_DESCRIPTION}"
    )
    if(NOT CMAKE_BUILD_TYPE)
      set(CMAKE_BUILD_TYPE
          Debug
          CACHE STRING "Build type" FORCE
      )
    endif()
    set(CMAKE_C_STANDARD 23)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_C_FLAGS_DEBUG "-O0 -g3 -DDEBUG")
    set(CMAKE_ASM_FLAGS_DEBUG "-g3")
    set(CMAKE_C_FLAGS_RELEASE "-Os -g1 -DNDEBUG")
    set(CMAKE_ASM_FLAGS_RELEASE "-g1")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "-Og -g3")
    set(CMAKE_ASM_FLAGS_RELWITHDEBINFO "-g3")
    option(RA8_TRUSTZONE_ENABLE "Enable Cortex-M85 TrustZone (-mcmse + SAU)" OFF)
  endif()

  # ---- vendored middleware (USES) ---------------------------------------
  # For each requested middleware: default RA8_USE_<M> ON in standalone
  # builds, skip the whole app if it is OFF, then pull in cmake/<m>.cmake
  # (which defines the interface library target <m>).
  foreach(_ra8_use ${_RA8_APP_USES})
    string(TOUPPER "${_ra8_use}" _ra8_use_up)
    if(NOT _ra8_has_parent)
      option(RA8_USE_${_ra8_use_up} "Enable the vendored ${_ra8_use}" ON)
    endif()
    if(NOT RA8_USE_${_ra8_use_up})
      message(STATUS "${_RA8_APP_NAME}: skipped (RA8_USE_${_ra8_use_up} is OFF)")
      return()
    endif()
    if(NOT TARGET ${_ra8_use})
      include(${RA8_REPO_ROOT}/cmake/${_ra8_use}.cmake OPTIONAL)
    endif()
  endforeach()

  # ---- insecure placeholder-crypto opt-in (issue #180) ------------------
  # Several secure-side TUs (src/secure_app/{secure_trng,key_import,key_vault}.c
  # and libs/ra8_hal/src/ra8_rsip_key_injection.c) ship an INSECURE placeholder
  # body (a deterministic PRNG "TRNG", a forgeable key-import MAC, a plain-SRAM
  # key vault, a non-cryptographic RSIP key-wrap) that is only safe under host
  # simulation or an explicitly-declared dev/eval image. Each such body is
  # wrapped in `#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_SIMULATOR_MODE)`
  # and its `#else` fails closed (every entry point returns a hard error). This
  # option is OFF by default so a release/HIL firmware image that forgets to
  # swap in a real crypto backend fails closed instead of silently shipping the
  # stub. A dev/eval firmware image opts in with -DRA8_INSECURE_STUB_CRYPTO=ON.
  option(RA8_INSECURE_STUB_CRYPTO "compile insecure placeholder crypto (host/sim only)" OFF)

  include(${RA8_REPO_ROOT}/cmake/ra8_warnings.cmake)
  include(${RA8_REPO_ROOT}/cmake/ra8_shared_libs.cmake)

  # The app source list and its LIBS expansion (cmake/ra8_app/sources.cmake).
  _ra8_app_collect_sources()
  # ---- prebuilt universal-library fast path (CI cross-build) ------------
  # When RA8_SHARED_LIB_ARCHIVE points at a prebuilt libra8_shared_<board>.a
  # (scripts/builders/all_examples.sh sets it for the "Cross-build all apps"
  # gate), link that archive instead of recompiling the ~180 universal
  # first-party sources (ra8_core / ra8_hal / ra8_net_pal / ra8_usb_pal /
  # board / secure_app) into THIS executable -- they are compiled once for
  # the whole cross-build. --whole-archive pulls every member object (so the
  # link is identical to compiling the sources in) and the toolchain's
  # --gc-sections then prunes the unused ones, yielding the same final ELF
  # sections. The archive is board-independent code + the ek_ra8d2 board, and
  # is built for the RA8D2 (Cortex-M85, fpv5-sp-d16, no device define) with
  # TrustZone OFF and the placeholder crypto OFF, so an app is eligible only
  # when its own config matches: any other config falls back to the normal
  # per-source compile so its objects stay correct and its gates (e.g. the
  # tz_nsc_cgc_usb SG-veneer offsets) stay meaningful. In particular the RA8P1
  # tier (cmake/toolchain-ra8p1.cmake) builds the SAME shared sources with a
  # DIFFERENT device (-DRA8_DEVICE_RA8P1 -> ra8_npu_loader.c and register
  # bases change) AND a different FP ABI (double-precision fpv5-d16), so its
  # objects are symbol- and ABI-incompatible with this RA8D2 archive -- it is
  # excluded by the device-define check below (those apps set no BOARD, so the
  # board check alone would wrongly admit them).
  set(_ra8_shared_archive "$ENV{RA8_SHARED_LIB_ARCHIVE}")
  set(_ra8_use_shared_archive FALSE)
  if(_ra8_shared_archive
     AND EXISTS "${_ra8_shared_archive}"
     AND _RA8_APP_BOARD STREQUAL "ek_ra8d2"
     AND NOT RA8_TRUSTZONE_ENABLE
     AND NOT RA8_INSECURE_STUB_CRYPTO
     AND NOT CMAKE_C_FLAGS MATCHES "RA8_DEVICE_RA8P1"
  )
    set(_ra8_use_shared_archive TRUE)
    # Drop the universal set from this app's own compile; the archive
    # supplies those objects. NSC (per-app subsets / -mcmse) and the extra
    # per-app libraries + SOUP TUs are NOT in the archive and stay here.
    set(_ra8_lib_core "")
    set(_ra8_lib_hal "")
    set(_ra8_lib_net_pal "")
    set(_ra8_lib_usb_pal "")
    set(_ra8_lib_board "")
    set(_ra8_secure_app "")
    # ~150 apps also name a universal-set library (usually
    # ra8_board_ek_ra8d2, some ra8_core / ra8_hal / ra8_usb_pal /
    # ra8_net_pal) explicitly in LIBS, which globbed its sources into
    # _ra8_lib_extra above -- a SECOND copy that would multiply-define
    # against the archive. Filter those exact universal source trees back
    # out of the extra list (their objects come from the archive; the
    # include paths the LIBS loop added stay). SIM_LIBS is never a
    # universal library, so _ra8_lib_extra_sim needs no filtering.
    foreach(
      _ra8_univ_frag
      "/libs/ra8_core/src/"
      "/libs/ra8_hal/src/"
      "/libs/ra8_net_pal/src/"
      "/libs/ra8_usb_pal/src/"
      "/libs/ra8_board_${_RA8_APP_BOARD}/src/"
      "/src/secure_app/"
    )
      list(
        FILTER
        _ra8_lib_extra
        EXCLUDE
        REGEX
        "${_ra8_univ_frag}"
      )
    endforeach()
  endif()

  add_executable(
    ${_ra8_elf}
    ${_ra8_src}
    ${_ra8_lib_core}
    ${_ra8_lib_hal}
    ${_ra8_lib_net_pal}
    ${_ra8_lib_usb_pal}
    ${_ra8_lib_nsc}
    ${_ra8_lib_board}
    ${_ra8_secure_app}
    ${_ra8_lib_extra}
    ${_ra8_lib_extra_sim}
  )

  if(_ra8_use_shared_archive)
    target_link_libraries(
      ${_ra8_elf} PRIVATE -Wl,--whole-archive ${_ra8_shared_archive} -Wl,--no-whole-archive
    )
    set_property(
      TARGET ${_ra8_elf}
      APPEND
      PROPERTY LINK_DEPENDS ${_ra8_shared_archive}
    )
  endif()

  if(_ra8_lib_extra_sim)
    set_source_files_properties(
      ${_ra8_lib_extra_sim} PROPERTIES COMPILE_DEFINITIONS "RA8_SIMULATOR_MODE"
    )
  endif()

  # Per-TU options for the vendored SOUP decoders (cmake/ra8_app/vendored.cmake).
  _ra8_app_vendored_flags()

  ra8_target_enable_project_warnings(${_ra8_elf} STACK_USAGE_BYTES ${_RA8_APP_STACK_BYTES})
  target_compile_options(${_ra8_elf} PRIVATE -fshort-enums)

  # Vendored RTOS / middleware headers trip several strict-warning gates we
  # apply to first-party code (CHAR* params, redundant decls, casts,
  # redefined macros). Relax the gate for apps that pull them in; the rest
  # of the codebase still gets the full -Werror set.
  if(_RA8_APP_USES)
    target_compile_options(
      ${_ra8_elf}
      PRIVATE -Wno-error=discarded-qualifiers
              -Wno-error=cast-qual
              -Wno-error=cast-align
              -Wno-error=redundant-decls
              -Wno-error=missing-prototypes
              -Wno-error=builtin-macro-redefined
              -Wno-error
    )
  endif()

  if(RA8_TRUSTZONE_ENABLE)
    target_compile_definitions(${_ra8_elf} PRIVATE RA8_TRUSTZONE_ENABLE)
    target_compile_options(${_ra8_elf} PRIVATE -mcmse)
    target_link_options(${_ra8_elf} PRIVATE -mcmse)
  endif()

  # Compile the insecure placeholder crypto only when explicitly opted in
  # (issue #180). Left OFF, the guarded #else branches fail closed.
  if(RA8_INSECURE_STUB_CRYPTO)
    target_compile_definitions(${_ra8_elf} PRIVATE RA8_INSECURE_STUB_CRYPTO)
  endif()

  target_include_directories(
    ${_ra8_elf}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${RA8_REPO_ROOT}/src
            ${RA8_REPO_ROOT}/src/inc
            ${RA8_REPO_ROOT}/src/secure_app
            ${RA8_REPO_ROOT}/src/secure_app/inc
            ${RA8_REPO_ROOT}/libs/ra8_core/inc
            ${RA8_REPO_ROOT}/libs/ra8_hal/inc
            ${RA8_REPO_ROOT}/libs/ra8_net_pal/inc
            ${RA8_REPO_ROOT}/libs/ra8_usb_pal/inc
            ${RA8_REPO_ROOT}/libs/ra8_nsc/inc
            ${_ra8_board_dir}/inc
            ${_ra8_lib_inc}
            ${_ra8_extra_inc}
  )

  # Each middleware <m> ships a board-port interface library <m>_port_<bus>
  # that carries the RA-specific bridge headers; link it alongside the core.
  set(_ra8_port_lib_threadx "")
  set(_ra8_port_lib_usbx usbx_port_ra8_usb)
  set(_ra8_port_lib_netxduo netxduo_port_ra8_eth)
  set(_ra8_port_lib_mbedtls mbedtls_port_ra8_config)
  set(_ra8_port_lib_filex filex_port_ra8_sdhi)
  set(_ra8_port_lib_levelx levelx_port_ra8_xspi)
  set(_ra8_port_lib_nimble nimble_port_threadx)

  # Link the middleware interface libraries + pull in their bridge sources.
  foreach(_ra8_use ${_RA8_APP_USES})
    string(TOUPPER "${_ra8_use}" _ra8_use_up)
    if(TARGET ${_ra8_use})
      target_link_libraries(${_ra8_elf} PRIVATE ${_ra8_use})
    endif()
    if(_ra8_port_lib_${_ra8_use} AND TARGET ${_ra8_port_lib_${_ra8_use}})
      target_link_libraries(${_ra8_elf} PRIVATE ${_ra8_port_lib_${_ra8_use}})
    endif()
    get_property(_ra8_port GLOBAL PROPERTY RA8_${_ra8_use_up}_PORT_SOURCES)
    if(_ra8_port)
      target_sources(${_ra8_elf} PRIVATE ${_ra8_port})
    endif()
  endforeach()

  target_link_options(${_ra8_elf} PRIVATE -T${_ra8_linker} -Wl,--Map=${_RA8_APP_NAME}.map)
  set_target_properties(${_ra8_elf} PROPERTIES LINK_DEPENDS ${_ra8_linker})

  add_custom_command(
    TARGET ${_ra8_elf}
    POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${_ra8_elf}> ${_RA8_APP_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${_ra8_elf}> ${_RA8_APP_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${_ra8_elf}>
    COMMENT "Generating ${_RA8_APP_NAME}.hex / .bin and showing size"
  )
endmacro()

# ============================================================================
# ra8_add_cpu1_image() -- embed a Cortex-M33 (CPU1) image into an M85 (CPU0) ELF
# ============================================================================
# Builds a second, freestanding Cortex-M33 executable from SOURCES, objcopies it
# to a raw .bin, repacks that as a relocatable `.cpu1_image` object, and links it
# into PARENT (the M85 app .elf). The app's linker_script.ld must pin
# `.cpu1_image` at ORIGIN(MRAM_CPU1) (0x020C0000) so a single SWD flash drops
# both cores' images. The M33 firmware exports `cpu1_reset_handler` and an
# 8-entry vector table at 0x020C0000; `ra8_cpu1_release()` (HUM Ch 2.9.1) starts
# it at runtime. Both RA8D2 cores are single-precision FP (fpv5-sp-d16).
#
# Generalises the copy-pasted second-executable + objcopy recipe so any app opts
# in an M33 image with one call. Cross-build only: on the host
# (RA8_SIMULATOR_MODE / __APPLE__) there is no arm-none-eabi toolchain, so this is
# a no-op and `make test` keeps building the M85 side alone.
#
# Usage:
#   ra8_add_cpu1_image(
#     PARENT   blink_m33            # M85 app target base name (-> <PARENT>.elf)
#     NAME     blink_m33_cpu1       # CPU1 target base name (-> <NAME>.elf/.bin)
#     SOURCES  cpu1_main.c          # M33 sources (relative to the app dir)
#     LINKER   linker_script_cpu1.ld  # optional; defaults to this name
#     INCLUDES ${EXTRA_INC} ...     # optional extra include dirs
#   )
function(ra8_add_cpu1_image)
  cmake_parse_arguments(
    C1
    ""
    "PARENT;NAME;LINKER"
    "SOURCES;INCLUDES"
    ${ARGN}
  )

  # Host build: no cross toolchain, so skip the M33 image entirely.
  if(NOT (CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_NAME STREQUAL "Generic"))
    return()
  endif()
  if(NOT C1_PARENT)
    message(FATAL_ERROR "ra8_add_cpu1_image(): PARENT (the M85 app target) is required")
  endif()
  if(NOT C1_NAME)
    set(C1_NAME ${C1_PARENT}_cpu1)
  endif()
  if(NOT C1_LINKER)
    set(C1_LINKER linker_script_cpu1.ld)
  endif()

  set(_c1_ld ${CMAKE_CURRENT_SOURCE_DIR}/${C1_LINKER})
  set(_c1_srcs "")
  foreach(_s ${C1_SOURCES})
    list(APPEND _c1_srcs ${CMAKE_CURRENT_SOURCE_DIR}/${_s})
  endforeach()

  # The M33 image: -mcpu=cortex-m33, no M85 startup (own cpu1_reset_handler),
  # freestanding, size-optimised to fit the 256 KiB MRAM_CPU1 region.
  add_executable(${C1_NAME}.elf ${_c1_srcs})
  target_compile_definitions(${C1_NAME}.elf PRIVATE RA8_BUILD_FOR_CPU1)
  target_compile_options(
    ${C1_NAME}.elf
    PRIVATE -mcpu=cortex-m33
            -mthumb
            -mfloat-abi=hard
            -mfpu=fpv5-sp-d16
            -ffreestanding
            -fno-builtin
            -fshort-enums
            -Os
            -g3
  )

  # The per-app Cortex-M33 entry (the helper's own SOURCES, e.g. cpu1_main.c)
  # was escaping the project warning / -Werror / stack-usage bar that every
  # M85 TU receives via ra8_target_enable_project_warnings(): this recipe set
  # only the cpu flags + -Os, so half the dual-core product compiled with zero
  # of -Wall / -Wextra / -Werror / -Wshadow / -Wstack-usage and the M33 side
  # emitted no .su stack data. Apply the same first-party warning set the M85
  # build uses (kept in step with cmake/ra8_warnings.cmake) plus -Wstack-usage
  # + -fstack-usage so cpu1_main.c is held to the same safety bar and feeds the
  # .su aggregator (scripts/checks/stack_usage_check.py).
  #
  # Applied PER-SOURCE to the helper SOURCES -- NOT target-level -- on purpose:
  # an app may bolt extra translation units onto the CPU1 elf with its own
  # target_sources() (e.g. compile_on_m33 adds the rabook XHTML pipeline plus
  # vendored miniz.c / tinyxml2.cpp with their own -w). A target-level -Werror
  # would force those onto the first-party bar (and -Wstack-usage onto the
  # multi-KiB XHTML walker frames), reddening the build. Per-source scoping
  # holds the lines this helper owns to the bar without touching the app's
  # bolted-on sources. TODO(T1-09): extend the bar to app-added first-party M33
  # TUs (ra8_gfx / ra8_ipc / ra8_rabook on the M33 side) via per-source opt-in in
  # those apps' own CMakeLists; those TUs are still -Werror-gated in the M85
  # builds where they are also compiled.
  # The extended warning profile, spelled as a list so no single line runs
  # past the 100-column limit. COMPILE_OPTIONS takes a ;-separated list,
  # which is exactly what set() builds from these arguments.
  set(_c1_warnings
      -Wall
      -Wextra
      -Werror
      -Wcast-qual
      -Wcast-align
      -Wdouble-promotion
      -Wformat=2
      -Wpointer-arith
      -Wshadow
      -Wundef
      -Wvla
      -Wwrite-strings
      -Wbad-function-cast
      -Wmissing-declarations
      -Wmissing-prototypes
      -Wnested-externs
      -Wold-style-definition
      -Wredundant-decls
      -Wstrict-prototypes
      -Wduplicated-branches
      -Wduplicated-cond
      -Wformat-overflow=2
      -Wformat-truncation=2
      -Wlogical-op
      -Wstack-usage=2048
      -fstack-usage
  )
  set_source_files_properties(${_c1_srcs} PROPERTIES COMPILE_OPTIONS "${_c1_warnings}")
  target_link_options(
    ${C1_NAME}.elf
    PRIVATE
    -mcpu=cortex-m33
    -mthumb
    -mfloat-abi=hard
    -mfpu=fpv5-sp-d16
    -nostartfiles
    -T${_c1_ld}
    -Wl,--Map=${C1_NAME}.map
  )
  target_include_directories(
    ${C1_NAME}.elf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${RA8_REPO_ROOT}/libs/ra8_core/inc
                           ${C1_INCLUDES}
  )
  set_target_properties(${C1_NAME}.elf PROPERTIES LINK_DEPENDS ${_c1_ld})
  add_custom_command(
    TARGET ${C1_NAME}.elf
    POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${C1_NAME}.elf> ${C1_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${C1_NAME}.elf> ${C1_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${C1_NAME}.elf>
    COMMENT "Generating ${C1_NAME}.hex / .bin (Cortex-M33 image) and showing size"
  )

  # Repack the CPU1 .bin as a relocatable `.cpu1_image` object and link it into
  # PARENT; the app linker script pins `.cpu1_image` at ORIGIN(MRAM_CPU1) so one
  # .hex spans MRAM (0x02000000) and MRAM_CPU1 (0x020C0000).
  set(_c1_bin ${CMAKE_CURRENT_BINARY_DIR}/${C1_NAME}.bin)
  set(_c1_obj ${CMAKE_CURRENT_BINARY_DIR}/${C1_NAME}_blob.o)
  add_custom_command(
    OUTPUT ${_c1_obj}
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm -B arm --rename-section
            .data=.cpu1_image,alloc,load,readonly,contents ${_c1_bin} ${_c1_obj}
    DEPENDS ${C1_NAME}.elf
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Packing ${C1_NAME}.bin as relocatable .cpu1_image object"
  )
  target_sources(${C1_PARENT}.elf PRIVATE ${_c1_obj})
  set_source_files_properties(${_c1_obj} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  add_dependencies(${C1_PARENT}.elf ${C1_NAME}.elf)
endfunction()
