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
# Or via the top-level Makefile:
#
#     make fuzz                # 30s smoke run of every harness
#
# Per-harness longer sessions:
#
#     bash scripts/utils/run_fuzz.sh fuzz_ra8_jpeg_sw 600
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
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
# which conflicts with the ra8_sim_mmap MAP_FIXED windows at
# 0x02000000..0x68000000. On macOS we drop ASan and rely on libFuzzer
# coverage + UBSan; the simulator's own MAP_FIXED guards still flag
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
# (ra8_core_hal MINUS tests/mocks/ra8_sim_mmap.c; defined in
# tests/CMakeLists.txt). That library is compiled WITHOUT the fuzzer
# sanitizer flags, so coverage feedback only spans the harness translation
# unit. ASan and UBSan still observe out-of-bounds reads / integer UB
# inside the linked-in production code, which is the primary value we are
# after.
#
# ra8_sim_mmap.c is excluded from that library because its load-time
# MAP_FIXED constructor maps 0xE0000000, which lands in ASan's shadow gap
# and aborts the process before main() (issue #193). The pure-computation
# harnesses -- parsers/codecs driven purely by their input buffer or by
# injected dependencies -- never touch peripheral MMIO, so they simply omit
# it. Only the harnesses in RA8_FUZZ_MMIO_TARGETS dereference real
# peripheral registers; they add ra8_sim_mmap.c back as a direct source, so
# it is compiled with THIS target's ASan flags and skips the shadow-gap
# region (ra8_sim_region_mappable()) instead of aborting.
set(RA8_FUZZ_TARGETS
    fuzz_ra8_jpeg_sw
    fuzz_ra8_jpeg_sw_block
    fuzz_ra8_epub
    fuzz_ra8_modem_at
    fuzz_ra8_ble_att
    fuzz_ra8_usb_pal
    fuzz_ra8_tls
    fuzz_ra8_canfd
    fuzz_ra8_etha
    fuzz_ra8_fs_fat
    fuzz_ra8_stb_image
    fuzz_ra8_reflow_xml
    fuzz_ra8_stbtt
    fuzz_ra8_rar
    fuzz_ra8_webp
    fuzz_ra8_jof
    fuzz_ra8_jof_produce
    fuzz_ra8_unarch_xz
    fuzz_ra8_unarch_tar
    fuzz_ra8_unarch_gzip
    fuzz_ra8_decomp_limits
)

# Harnesses that dereference real peripheral registers and therefore need
# the ra8_sim_mmap host backing store. All touch only peripheral-bus
# addresses (0x40000000-class), never the 0xE0000000 SCB/MPU window, so the
# shadow-gap skip in ra8_sim_mmap.c does not affect them:
#   fuzz_ra8_canfd    -- stages a CFDRF[0] RX block and decodes via ra8_canfd_receive
#   fuzz_ra8_usb_pal  -- ra8_mstp_init + ra8_usb_pal_init / _ep_* touch USBFS regs
#   fuzz_ra8_ble_att  -- ra8_ble_host_init -> ra8_ble_open writes the BLE
#                       controller block at 0x40700000
set(RA8_FUZZ_MMIO_TARGETS fuzz_ra8_canfd fuzz_ra8_usb_pal fuzz_ra8_ble_att)

foreach(_t ${RA8_FUZZ_TARGETS})
  # fuzz_log_sink.c installs a discard ra8_log byte sink before main() so no
  # harness reaches the ITM registers at 0xE0000000 (which are unmapped
  # here -- see fuzz_log_sink.c and issue #193). Linked into every harness.
  set(_t_srcs ${CMAKE_CURRENT_SOURCE_DIR}/${_t}.c ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_log_sink.c)
  if(_t IN_LIST RA8_FUZZ_MMIO_TARGETS)
    list(APPEND _t_srcs ${CMAKE_CURRENT_SOURCE_DIR}/../mocks/ra8_sim_mmap.c)
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
            ${FW_ROOT}/libs/ra8_epub/inc
            ${FW_ROOT}/libs/ra8_ble_host/inc
            ${FW_ROOT}/libs/ra8_usb_pal/inc
            ${FW_ROOT}/libs/ra8_tls/inc
            ${FW_ROOT}/libs/ra8_fs/inc
            ${FW_ROOT}/libs/ra8_reflow/inc
            ${FW_ROOT}/libs/ra8_comic/inc
            ${FW_ROOT}/libs/ra8_unarch/inc
            ${FW_ROOT}/libs/ra8_webp/inc
            ${FW_ROOT}/libs/ra8_jof/inc
            ${FW_ROOT}/libs/third_party/stb
            ${FW_ROOT}/libs/third_party/xz_embedded
            # tests/ root: shared test fixtures (e.g. eth_frame_fixture.h,
            # the fuzz_ra8_etha parse target).
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/../mocks
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
# (re-)materialised by scripts/utils/init_fuzz_corpora.sh. Both
# `make fuzz` and scripts/utils/run_fuzz.sh invoke that script
# before running a harness, then pass the corpus directory as a
# positional argument to libFuzzer. See docs/FUZZING.md.
add_custom_target(
  ra8_fuzz_seed_corpora
  COMMAND bash ${FW_ROOT}/scripts/utils/init_fuzz_corpora.sh
  WORKING_DIRECTORY ${FW_ROOT}
  COMMENT "Seeding libFuzzer corpora under tests/fuzz/corpus/"
)
