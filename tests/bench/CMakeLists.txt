# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# tests/bench/CMakeLists.txt -- host-side performance microbenchmarks.
#
# Opt-in via -DRA8_BENCH=ON so the normal host test build (and ctest in
# CI) is unaffected. Each bench target is a standalone executable that
# links against the existing ra8_core_hal OBJECT library and writes CSV
# results to raw descriptor 1. See docs/PERFORMANCE.md for how to run and what
# the columns mean.
#
# Build + run via:
#
#     cmake -S tests -B tests/build-bench -DRA8_BENCH=ON
#     cmake --build tests/build-bench --target ra8_bench_all
#     just quality::local::bench  # convenience wrapper
#
#

if(NOT RA8_BENCH)
  return()
endif()

message(STATUS "RA8_BENCH=ON: building host-side performance benchmarks")

# Bench targets are compiled with -O2 -DNDEBUG so the timing reflects
# release-style code generation rather than the -O0 -g host test build.
# We deliberately do NOT add coverage/MC-DC instrumentation to bench
# binaries; mixing the two produces meaningless numbers.
set(RA8_BENCH_TARGETS bench_ra8_crc bench_ra8_jpeg_sw bench_ra8_gfx_text)

foreach(_t ${RA8_BENCH_TARGETS})
  add_executable(${_t} ${CMAKE_CURRENT_SOURCE_DIR}/src/${_t}.c $<TARGET_OBJECTS:ra8_core_hal>)
  target_compile_options(${_t} PRIVATE -O2 -DNDEBUG -Wall -Wextra)
  target_include_directories(
    ${_t}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/libs/ra8_hal/inc
            ${FW_ROOT}/libs/ra8_gfx/inc
            ${CMAKE_CURRENT_SOURCE_DIR}/../mocks/inc
  )
  set_target_properties(${_t} PROPERTIES LINKER_LANGUAGE CXX)
  if(APPLE)
    # Same MAP_FIXED-friendly link layout the unit-test executables
    # use: shrink __PAGEZERO down to 16 KiB so the fake MMIO
    # windows under 0x80000000 can be mapped on macOS arm64.
    target_link_options(${_t} PRIVATE "-Wl,-pagezero_size,0x4000" "-Wl,-segalign,0x4000")
    # Re-codesign after the linker emits the binary; macOS sometimes
    # rejects adhoc-signed binaries whose pagezero has been resized
    # by the linker. Re-running `codesign --force --sign -` fixes
    # the kernel kill (SIGKILL with no stderr).
    add_custom_command(
      TARGET ${_t}
      POST_BUILD
      COMMAND codesign --force --sign - $<TARGET_FILE:${_t}>
      COMMENT "Re-codesigning ${_t} after the pagezero resize"
      VERBATIM
    )
  endif()
endforeach()

# The software-JPEG bench includes its public decoder contract directly.
target_include_directories(bench_ra8_jpeg_sw PRIVATE ${FW_ROOT}/libs/ra8_jpeg/inc)

# The deterministic output golden needs no production object library: it
# exercises only the caller-owned RAM sink and typed benchmark renderer.
add_executable(test_ra8_bench_output ${CMAKE_CURRENT_SOURCE_DIR}/src/test_ra8_bench_output.c)
target_compile_options(test_ra8_bench_output PRIVATE -Wall -Wextra -Werror)
target_include_directories(
  test_ra8_bench_output PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${FW_ROOT}/libs/ra8_core/inc
)
add_test(NAME test_ra8_bench_output COMMAND test_ra8_bench_output)
list(APPEND RA8_BENCH_TARGETS test_ra8_bench_output)

# Convenience aggregate target -- `cmake --build . --target ra8_bench_all`.
add_custom_target(
  ra8_bench_all
  DEPENDS ${RA8_BENCH_TARGETS}
  COMMENT "Building every ra8 benchmark harness"
)
