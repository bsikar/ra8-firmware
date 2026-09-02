# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/ra8_shared_libs.cmake -- prebuilt "universal" first-party library set.
#
# Every example app compiles the SAME first-party library sources
# (ra8_core, ra8_hal, ra8_net_pal, ra8_usb_pal, the board layer, and the
# ra8_secure_app substrate) into its own executable via cmake/ra8_add_app.cmake.
# In the CI cross-build that is ~180 identical source files recompiled once
# per app across ~200 apps -- the dominant cost of the "Cross-build all apps"
# gate. Nothing about those objects varies between the eligible apps (same
# flags, same include roots, board-independent code), so they can be compiled
# ONCE into a static archive and linked into every eligible app.
#
# This file is the single source of truth for two things, shared between the
# archive builder (cmake/shared_libs/CMakeLists.txt) and the per-app recipe
# (cmake/ra8_add_app.cmake) so the two never drift:
#
#   ra8_shared_lib_sources(<out> <repo_root> <board_dir>)
#       the universal source list an app would otherwise compile itself.
#
#   ra8_shared_lib_includes(<out> <repo_root> <board_dir>)
#       the (non-app-specific) include roots those sources need.
#
# The NSC veneers (libs/ra8_nsc) are deliberately NOT in the universal set:
# apps select subsets (NSC_SRCS), exclude them (NO_NSC), or compile them under
# -mcmse (TrustZone), so they stay per-app.
#
#

# Canonical universal first-party source list. MUST stay in lockstep with the
# _ra8_lib_core / _ra8_lib_hal / _ra8_lib_net_pal / _ra8_lib_usb_pal /
# _ra8_lib_board / _ra8_secure_app globs in cmake/ra8_add_app.cmake -- an app
# that links the archive drops exactly these from its own compile, so a
# divergence surfaces as a loud undefined-symbol link error, never silent
# breakage.
function(ra8_shared_lib_sources out_var repo_root board_dir)
  file(GLOB_RECURSE _core CONFIGURE_DEPENDS ${repo_root}/libs/ra8_core/src/*.c)
  file(GLOB_RECURSE _hal CONFIGURE_DEPENDS ${repo_root}/libs/ra8_hal/src/*.c)
  file(GLOB_RECURSE _net_pal CONFIGURE_DEPENDS ${repo_root}/libs/ra8_net_pal/src/*.c)
  file(GLOB_RECURSE _usb_pal CONFIGURE_DEPENDS ${repo_root}/libs/ra8_usb_pal/src/*.c)
  # Boot composition sources live in src/boot and are selected per image by
  # ra8_app/sources.cmake. The shared archive owns only host-neutral board
  # implementations directly under src/.
  file(GLOB _board CONFIGURE_DEPENDS ${board_dir}/src/*.c)
  file(GLOB_RECURSE _secure CONFIGURE_DEPENDS ${repo_root}/libs/ra8_secure_app/src/*.c)
  set(${out_var}
      ${_core}
      ${_hal}
      ${_net_pal}
      ${_usb_pal}
      ${_board}
      ${_secure}
      PARENT_SCOPE
  )
endfunction()

# Canonical include roots for the universal sources. These mirror the
# non-app-specific entries in ra8_add_app()'s target_include_directories --
# the app dir / app inc / app src / extra include roots are intentionally
# absent: a universal library source that needed an app-local header would be
# a layering violation.
function(ra8_shared_lib_includes out_var repo_root board_dir)
  set(${out_var}
      ${repo_root}/libs/ra8_core/inc
      ${repo_root}/libs/ra8_hal/inc
      ${repo_root}/libs/ra8_net_pal/inc
      ${repo_root}/libs/ra8_usb_pal/inc
      ${repo_root}/libs/ra8_io/inc
      ${repo_root}/libs/ra8_nsc/inc
      ${repo_root}/libs/ra8_secure_app/inc
      ${board_dir}/inc
      PARENT_SCOPE
  )
endfunction()

# Build the universal static archive `ra8_shared_<board>` under the current
# project. Applies the EXACT compile surface an app applies to these sources:
# the shared project warning profile (with -fstack-usage so the .su stack data
# is still emitted -- the stack_usage_check.py gate reads it from this build
# tree) plus -fshort-enums. STACK_USAGE_BYTES matches ra8_add_app()'s default
# (2200): every eligible app currently compiles these sources at >= that
# budget, so the archive can never trip a -Wstack-usage error the per-app
# build would not have.
function(ra8_add_shared_lib_archive)
  cmake_parse_arguments(
    _SL
    ""
    "NAME;REPO_ROOT;BOARD;STACK_BYTES"
    ""
    ${ARGN}
  )
  if(NOT _SL_NAME)
    message(FATAL_ERROR "ra8_add_shared_lib_archive(): NAME is required")
  endif()
  if(NOT _SL_REPO_ROOT)
    message(FATAL_ERROR "ra8_add_shared_lib_archive(): REPO_ROOT is required")
  endif()
  if(NOT _SL_BOARD)
    set(_SL_BOARD "ek_ra8d2")
  endif()
  if(NOT _SL_STACK_BYTES)
    set(_SL_STACK_BYTES 2200)
  endif()

  set(_board_dir "${_SL_REPO_ROOT}/libs/ra8_board_${_SL_BOARD}")
  if(NOT EXISTS "${_board_dir}")
    message(
      FATAL_ERROR "ra8_add_shared_lib_archive(): BOARD '${_SL_BOARD}' has no layer at ${_board_dir}"
    )
  endif()

  include(${_SL_REPO_ROOT}/cmake/ra8_warnings.cmake)

  ra8_shared_lib_sources(_srcs ${_SL_REPO_ROOT} ${_board_dir})
  ra8_shared_lib_includes(_incs ${_SL_REPO_ROOT} ${_board_dir})

  add_library(${_SL_NAME} STATIC ${_srcs})
  ra8_target_enable_project_warnings(${_SL_NAME} STACK_USAGE_BYTES ${_SL_STACK_BYTES})
  target_compile_options(${_SL_NAME} PRIVATE -fshort-enums)
  target_include_directories(${_SL_NAME} PRIVATE ${_incs})
endfunction()
