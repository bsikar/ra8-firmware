# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# tests/fuzz/CMakeLists.txt -- libFuzzer harnesses for the parsers most
# exposed to untrusted input. Opt-in via -DRA8_FUZZ=ON so the normal
# host test build (and ctest in CI) is unaffected.
#
# Usage from the firmware root:
#
#     cmake -S tests -B tests/build-fuzz \
#         -DRA8_FUZZ=ON \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#     cmake --build tests/build-fuzz --target ra8_fuzz_all
#
# Or via the top-level justfile:
#
#     just quality::local::fuzz  # 30s smoke run of every harness
#
# Per-harness longer sessions:
#
#     bash scripts/checks/run_fuzz.sh fuzz_ra8_jpeg_sw 600
#
#

if(NOT RA8_FUZZ)
  return()
endif()

# libFuzzer is a clang-only feature. Refuse to configure if the active
# C compiler is not clang -- gcc has no `-fsanitize=fuzzer` support.
if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" AND NOT CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
  message(
    FATAL_ERROR "RA8_FUZZ=ON requires a clang C compiler " "(detected: ${CMAKE_C_COMPILER_ID}). "
                "Re-run cmake with -DCMAKE_C_COMPILER=clang " "-DCMAKE_CXX_COMPILER=clang++ ."
  )
endif()

message(STATUS "RA8_FUZZ=ON: building libFuzzer harnesses with "
               "-fsanitize=fuzzer,address,undefined"
)

# AddressSanitizer reserves the low 4 GB of address space on macOS,
# which conflicts with the ra8_fake_mmap MAP_FIXED windows at
# 0x02000000..0x68000000. On macOS we drop ASan and rely on libFuzzer
# coverage + UBSan; the fake's own MAP_FIXED guards still flag
# bad accesses. Linux has no such conflict and gets the full set.
if(APPLE)
  set(RA8_FUZZ_SAN_FLAGS
      -fsanitize=fuzzer,undefined
      -fno-sanitize-recover=undefined
      -fno-omit-frame-pointer
      -g
      -O1
  )
else()
  set(RA8_FUZZ_SAN_FLAGS
      -fsanitize=fuzzer,address,undefined
      -fno-sanitize-recover=all
      -fno-omit-frame-pointer
      -g
      -O1
  )
endif()

# Each fuzz target links against the slim ra8_core_hal_fuzz OBJECT library
# (ra8_core_hal MINUS tests/mocks/src/ra8_fake_mmap.c; defined in
# tests/CMakeLists.txt). That library is compiled WITHOUT the fuzzer
# sanitizer flags, so coverage feedback only spans the harness translation
# unit. ASan and UBSan still observe out-of-bounds reads / integer UB
# inside the linked-in production code, which is the primary value we are
# after.
#
# ra8_fake_mmap.c is excluded from that library because its load-time
# MAP_FIXED constructor maps 0xE0000000, which lands in ASan's shadow gap
# and aborts the process before main() (issue #193). The pure-computation
# harnesses -- parsers/codecs driven purely by their input buffer or by
# injected dependencies -- never touch peripheral MMIO, so they simply omit
# it. Only the harnesses in RA8_FUZZ_MMIO_TARGETS dereference real
# peripheral registers; they add ra8_fake_mmap.c back as a direct source, so
# it is compiled with THIS target's ASan flags and skips the shadow-gap
# region (ra8_fake_region_mappable()) instead of aborting.
set(RA8_FUZZ_TARGETS
    fuzz_ra8_jpeg_sw
    fuzz_ra8_jpeg_sw_block
    fuzz_epub
    fuzz_ra8_modem_at
    fuzz_ra8_usb_pal
    fuzz_ra8_tls
    fuzz_ra8_canfd
    fuzz_ra8_etha
    fuzz_ra8_fs_fat
    fuzz_ra8_stb_image
    fuzz_reflow_xml
    fuzz_ra8_stbtt
    fuzz_ra8_rar
    fuzz_ra8_webp
    fuzz_jof
    fuzz_jof_produce
    fuzz_unarch_xz
    fuzz_unarch_tar
    fuzz_unarch_gzip
    fuzz_ra8_decomp_limits
)

# Harnesses that dereference real peripheral registers and therefore need
# the ra8_fake_mmap host backing store. All touch only peripheral-bus
# addresses (0x40000000-class), never the 0xE0000000 SCB/MPU window, so the
# shadow-gap skip in ra8_fake_mmap.c does not affect them:
#   fuzz_ra8_canfd    -- stages a CFDRF[0] RX block and decodes via ra8_canfd_receive
#   fuzz_ra8_usb_pal  -- ra8_mstp_init + ra8_usb_pal_init / _ep_* touch USBFS regs
set(RA8_FUZZ_MMIO_TARGETS fuzz_ra8_canfd fuzz_ra8_usb_pal)

file(GLOB_RECURSE RA8_APP_FUZZ_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/apps/fuzz_*.c)
list(
  FILTER
  RA8_APP_FUZZ_SOURCES
  INCLUDE
  REGEX
  "/tests/src/fuzz_[^/]+\\.c$"
)

foreach(_t ${RA8_FUZZ_TARGETS})
  # fuzz_log_sink.c installs a discard ra8_log byte sink before main() so no
  # harness reaches the ITM registers at 0xE0000000 (which are unmapped
  # here -- see fuzz_log_sink.c and issue #193). Linked into every harness.
  set(_t_src ${CMAKE_CURRENT_SOURCE_DIR}/src/${_t}.c)
  if(NOT EXISTS ${_t_src})
    set(_t_matches "")
    foreach(_candidate IN LISTS RA8_APP_FUZZ_SOURCES)
      get_filename_component(_candidate_name ${_candidate} NAME_WE)
      if(_candidate_name STREQUAL _t)
        list(APPEND _t_matches ${_candidate})
      endif()
    endforeach()
    list(LENGTH _t_matches _t_match_count)
    if(NOT _t_match_count EQUAL 1)
      message(FATAL_ERROR "RA8_FUZZ_TARGETS entry ${_t} resolves to ${_t_match_count} sources")
    endif()
    list(GET _t_matches 0 _t_src)
  endif()
  set(_t_srcs ${_t_src} ${CMAKE_CURRENT_SOURCE_DIR}/src/fuzz_log_sink.c)
  if(_t IN_LIST RA8_FUZZ_MMIO_TARGETS)
    list(APPEND _t_srcs ${CMAKE_CURRENT_SOURCE_DIR}/../mocks/src/ra8_fake_mmap.c)
  endif()
  add_executable(${_t} ${_t_srcs} $<TARGET_OBJECTS:ra8_core_hal_fuzz>)
  target_compile_options(${_t} PRIVATE ${RA8_FUZZ_SAN_FLAGS})
  target_link_options(${_t} PRIVATE ${RA8_FUZZ_SAN_FLAGS})
  target_include_directories(
    ${_t}
    PRIVATE ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/libs/ra8_hal/inc
            ${FW_ROOT}/libs/ra8_modem_at/inc
            ${FW_ROOT}/libs/ra8_net_pal/inc
            ${FW_ROOT}/apps/shared_libs/epub/inc
            ${FW_ROOT}/apps/shared_libs/epub/src
            ${FW_ROOT}/apps/shared_libs/xml/inc
            ${FW_ROOT}/libs/ra8_jpeg/inc
            ${FW_ROOT}/libs/ra8_usb_pal/inc
            ${FW_ROOT}/libs/ra8_tls/inc
            ${FW_ROOT}/libs/ra8_fs/inc
            ${FW_ROOT}/apps/shared_libs/reflow/inc
            ${FW_ROOT}/apps/shared_libs/comic/inc
            ${FW_ROOT}/apps/shared_libs/unarch/inc
            ${FW_ROOT}/apps/shared_libs/webp/inc
            ${FW_ROOT}/apps/shared_libs/jof/inc
            ${FW_ROOT}/apps/shared_libs/third_party/stb
            ${FW_ROOT}/apps/shared_libs/third_party/xz_embedded
            ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/inc
            ${CMAKE_CURRENT_SOURCE_DIR}/../mocks/inc
            ${CMAKE_CURRENT_SOURCE_DIR}/../support/inc
  )
  set_target_properties(${_t} PROPERTIES LINKER_LANGUAGE CXX)
endforeach()

# Convenience aggregate target: `cmake --build . --target ra8_fuzz_all`.
add_custom_target(
  ra8_fuzz_all
  DEPENDS ${RA8_FUZZ_TARGETS}
  COMMENT "Building every libFuzzer harness"
)

# Seed corpora live under tests/fuzz/corpus/<target>/ and are
# (re-)materialised by scripts/builders/init_fuzz_corpora.sh. Both
# `just quality::local::fuzz` and scripts/checks/run_fuzz.sh invoke that script
# before running a harness, then pass the corpus directory as a
# positional argument to libFuzzer. See docs/FUZZING.md.
add_custom_target(
  ra8_fuzz_seed_corpora
  COMMAND bash ${FW_ROOT}/scripts/builders/init_fuzz_corpora.sh
  WORKING_DIRECTORY ${FW_ROOT}
  COMMENT "Seeding libFuzzer corpora under tests/fuzz/corpus/"
)
