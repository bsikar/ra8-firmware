#
# tests/fuzz/CMakeLists.txt -- libFuzzer harnesses for the parsers most
# exposed to untrusted input. Opt-in via -DRA_FUZZ=ON so the normal
# host test build (and ctest in CI) is unaffected.
#
# Usage from the firmware root:
#
#     cmake -S tests -B tests/build-fuzz \
#         -DRA_FUZZ=ON \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#     cmake --build tests/build-fuzz --target ra_fuzz_all
#
# Or via the top-level Makefile:
#
#     make fuzz                # 30s smoke run of every harness
#
# Per-harness longer sessions:
#
#     bash scripts/utils/run_fuzz.sh fuzz_ra_jpeg_sw 600
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

if(NOT RA_FUZZ)
  return()
endif()

# libFuzzer is a clang-only feature. Refuse to configure if the active
# C compiler is not clang -- gcc has no `-fsanitize=fuzzer` support.
if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang"
   AND NOT CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
  message(FATAL_ERROR
    "RA_FUZZ=ON requires a clang C compiler "
    "(detected: ${CMAKE_C_COMPILER_ID}). "
    "Re-run cmake with -DCMAKE_C_COMPILER=clang "
    "-DCMAKE_CXX_COMPILER=clang++ .")
endif()

message(STATUS "RA_FUZZ=ON: building libFuzzer harnesses with "
               "-fsanitize=fuzzer,address,undefined")

# AddressSanitizer reserves the low 4 GB of address space on macOS,
# which conflicts with the ra_sim_mmap MAP_FIXED windows at
# 0x02000000..0x68000000. On macOS we drop ASan and rely on libFuzzer
# coverage + UBSan; the simulator's own MAP_FIXED guards still flag
# bad accesses. Linux has no such conflict and gets the full set.
if(APPLE)
  set(RA_FUZZ_SAN_FLAGS
      -fsanitize=fuzzer,undefined
      -fno-sanitize-recover=undefined
      -fno-omit-frame-pointer
      -g -O1
  )
else()
  set(RA_FUZZ_SAN_FLAGS
      -fsanitize=fuzzer,address,undefined
      -fno-sanitize-recover=all
      -fno-omit-frame-pointer
      -g -O1
  )
endif()

# Each fuzz target links against the existing ra_core_hal OBJECT
# library. That library is compiled WITHOUT the fuzzer sanitizer flags,
# so coverage feedback only spans the harness translation unit. ASan
# and UBSan still observe out-of-bounds reads / integer UB inside the
# linked-in production code, which is the primary value we are after.
set(RA_FUZZ_TARGETS
    fuzz_ra_jpeg_sw
    fuzz_ra_epub
    fuzz_ra_modem_at
    fuzz_ra_net_arp
    fuzz_ra_net_ipv4
)

foreach(_t ${RA_FUZZ_TARGETS})
  add_executable(${_t}
      ${CMAKE_CURRENT_SOURCE_DIR}/${_t}.c
      $<TARGET_OBJECTS:ra_core_hal>
  )
  target_compile_options(${_t} PRIVATE ${RA_FUZZ_SAN_FLAGS})
  target_link_options(${_t}    PRIVATE ${RA_FUZZ_SAN_FLAGS})
  target_include_directories(${_t} PRIVATE
      ${FW_ROOT}/libs/ra_core/inc
      ${FW_ROOT}/libs/ra_hal/inc
      ${FW_ROOT}/libs/ra_modem_at/inc
      ${FW_ROOT}/libs/ra_net_pal/inc
      ${FW_ROOT}/libs/ra_net/inc
      ${FW_ROOT}/libs/ra_epub/inc
      ${CMAKE_CURRENT_SOURCE_DIR}/../mocks
  )
  set_target_properties(${_t} PROPERTIES LINKER_LANGUAGE CXX)
endforeach()

# Convenience aggregate target: `cmake --build . --target ra_fuzz_all`.
add_custom_target(ra_fuzz_all DEPENDS ${RA_FUZZ_TARGETS})

# Seed corpora live under tests/fuzz/corpus/<target>/ and are
# (re-)materialised by scripts/utils/init_fuzz_corpora.sh. Both
# `make fuzz` and scripts/utils/run_fuzz.sh invoke that script
# before running a harness, then pass the corpus directory as a
# positional argument to libFuzzer. See docs/FUZZING.md.
add_custom_target(ra_fuzz_seed_corpora
    COMMAND bash ${FW_ROOT}/scripts/utils/init_fuzz_corpora.sh
    WORKING_DIRECTORY ${FW_ROOT}
    COMMENT "Seeding libFuzzer corpora under tests/fuzz/corpus/")
