# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/ra8_webp_vendor.cmake -- single source of truth for the WebP decode stack.
#
# Compiling WebP support is a four-part recipe, and every part is easy to get
# silently wrong:
#
#   1. the vendored decoder TUs      apps/shared_libs/third_party/libwebp/src/*.c
#   2. the include ROOT              apps/shared_libs/third_party/libwebp
#                                    because libwebp includes its own headers as
#                                    "src/webp/decode.h"
#   3. -DRA8_WEBP_USE_ARENA on (2)   activates the RA8 LOCAL PATCH in utils.c that
#                                    routes WebPSafe{Malloc,Calloc,Free} through the
#                                    heap-free ra8_webp bump arena instead of libc
#                                    malloc. Omit it and the "zero dynamic memory"
#                                    property (NASA P10 Rule 3) is silently lost.
#   4. -w;-fno-strict-aliasing on (2)  a 60+ TU codec whose per-TU warning tuning is
#                                    not tractable, and which type-puns through byte
#                                    buffers.
#
# Before this file existed the recipe was open-coded twice (cmake/ra8_add_app.cmake
# for firmware apps, tests/CMakeLists.txt for the host test build) and neither copy
# was reachable from a standalone host tool: ra8_add_app() is a whole firmware-app
# recipe (ARM toolchain, linker script, vector table) and the test build is one
# monolithic target. A host tool that hand-lists firmware sources therefore had no
# way to say "and WebP too" short of re-deriving all four parts by hand -- so
# tools/rabook_imagepack and apps/host/mdl instead each defined a do-nothing
# jof_priv_webp_transcode() that returned k_ra8_err_not_supported purely to
# satisfy the linker. Both tools linked clean, advertised WebP, and failed at
# runtime on every WebP source. This module removes the reason those stubs existed:
# the recipe now has exactly one definition that firmware apps, the host test
# build, and standalone host tools all call.
#
# API:
#   ra8_webp_vendor_sources(<out> <repo_root>)   vendored libwebp decoder TUs
#   ra8_webp_facade_sources(<out> <repo_root>)   first-party ra8_webp facade + arena
#   ra8_webp_includes(<out> <repo_root>)         include roots for both of the above
#   ra8_webp_apply_soup_flags(<sources>...)      per-source SOUP flags for part 3 + 4
#   ra8_webp_attach(<target> <repo_root>)        all of the above, applied to a target
#
# Consumers that assemble their own source lists (ra8_add_app, tests) call the
# granular accessors; a standalone host tool calls ra8_webp_attach() and is done.
#
#

# The vendored libwebp decoder (SOUP). Only the decode-only subset is vendored --
# src/enc/ carries headers alone -- so a recursive *.c glob is exactly that subset.
function(ra8_webp_vendor_sources out_var repo_root)
  set(_root "${repo_root}/apps/shared_libs/third_party/libwebp")
  if(NOT EXISTS "${_root}")
    message(FATAL_ERROR "ra8_webp_vendor_sources(): vendored libwebp missing at ${_root}")
  endif()
  file(GLOB_RECURSE _srcs CONFIGURE_DEPENDS ${_root}/src/*.c)
  if(NOT _srcs)
    message(FATAL_ERROR "ra8_webp_vendor_sources(): no decoder TUs under ${_root}/src")
  endif()
  set(${out_var}
      ${_srcs}
      PARENT_SCOPE
  )
endfunction()

# The first-party facade (ra8_webp.c) and its bump arena (ra8_webp_arena.c).
# Held to the full project warning bar -- these never take the SOUP flags.
function(ra8_webp_facade_sources out_var repo_root)
  set(_root "${repo_root}/apps/shared_libs/webp")
  if(NOT EXISTS "${_root}")
    message(FATAL_ERROR "ra8_webp_facade_sources(): ra8_webp facade missing at ${_root}")
  endif()
  file(GLOB_RECURSE _srcs CONFIGURE_DEPENDS ${_root}/src/*.c)
  if(NOT _srcs)
    message(FATAL_ERROR "ra8_webp_facade_sources(): no facade TUs under ${_root}/src")
  endif()
  set(${out_var}
      ${_srcs}
      PARENT_SCOPE
  )
endfunction()

# Include roots. libwebp's ROOT (not its src/) is the include dir: the codec
# includes its own headers as "src/webp/decode.h".
function(ra8_webp_includes out_var repo_root)
  set(${out_var}
      ${repo_root}/apps/shared_libs/third_party/libwebp ${repo_root}/apps/shared_libs/webp/inc
      ${repo_root}/apps/shared_libs/webp/src
      PARENT_SCOPE
  )
endfunction()

# Parts 3 + 4 of the recipe, applied to the vendored TUs only. RA8_WEBP_USE_ARENA
# is load-bearing for the zero-heap property, not a tuning knob -- see
# docs/SOUP/libwebp.md.
#
# This list is shared by three lanes -- the host unit-test build
# (tests/cmake/core_hal.cmake), the host tools, and every firmware app that
# links the decoder -- so it was measured on all three: gcc 14.2.0 and clang 18
# on the host (504 compiles each) and arm-none-eabi-gcc 13.3.1 cross (378), each
# flag removed on its own with the rest still applied, at -O0 and again at -Os.
# The blanket -Wno-error is gone: it was absorbing exactly two classes, and both
# are now named. -Wredundant-decls fires on the host lane
# (dsp/upsampling_sse2.c redeclares WebPUpsamplers) and -Wstack-usage= on the
# cross lane (utils/palette.c takes 5176 bytes against the 2200-byte per-app
# budget). Three more names masked nothing on any lane and are deleted:
# -Wno-sign-conversion and -Wno-implicit-int-conversion are both covered by
# -Wno-conversion, and -Wno-undef never fired at all.
function(ra8_webp_apply_soup_flags)
  if(NOT ARGN)
    message(FATAL_ERROR "ra8_webp_apply_soup_flags(): no sources given")
  endif()
  set(_ra8_wno_5
      -Wno-conversion # libwebp narrowing (utils.h int -> uint8_t clamp)
      -Wno-cast-align # dec/frame_dec.c casts uint8_t* to a wider element type
      -Wno-cast-qual # dec/idec_dec.c casts away const on the input buffer
      -Wno-double-promotion # utils/random_utils.c float -> double
      -Wno-bad-function-cast # dec/vp8l_dec.c casts a uint32_t call result
      -Wno-redundant-decls # host lane: dsp/upsampling_sse2.c redeclares WebPUpsamplers
      -fno-strict-aliasing
      -DRA8_WEBP_USE_ARENA
  )
  # -Wstack-usage= exists only on GCC, and clang REJECTS an unknown -Wno-<name>
  # under -Werror (-Wunknown-warning-option), so it is appended by compiler id
  # rather than left to a blanket -Wno-error to cover. On the cross lane it is
  # what utils/palette.c needs: a 5176-byte frame against the 2200-byte per-app
  # -Wstack-usage budget. -fstack-usage still emits the .su data, so
  # scripts/checks/stack_usage_check.py still sees the frame.
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND _ra8_wno_5 -Wno-stack-usage) # utils/palette.c has a measured 5176-byte SOUP frame.
  endif()
  set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS "${_ra8_wno_5}")
endfunction()

# One-call path for a standalone consumer (host tools): add the vendored decoder
# and the first-party facade to <target>, wire the include roots, and apply the
# SOUP flags to exactly the vendored TUs.
#
# Callers that also compile the jof producer get its WebP arm
# (jof_produce_webp.c) satisfied by this facade -- that arm is the
# jof_priv_webp_transcode() definition, and compiling it alongside this call is
# what makes WebP sources actually transcode rather than fail closed.
function(ra8_webp_attach target repo_root)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "ra8_webp_attach(): '${target}' is not a target")
  endif()
  ra8_webp_vendor_sources(_vendor ${repo_root})
  ra8_webp_facade_sources(_facade ${repo_root})
  ra8_webp_includes(_incs ${repo_root})
  target_sources(${target} PRIVATE ${_vendor} ${_facade})
  target_include_directories(${target} PRIVATE ${_incs})
  ra8_webp_apply_soup_flags(${_vendor})
endfunction()
