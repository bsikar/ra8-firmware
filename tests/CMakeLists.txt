cmake_minimum_required(VERSION 3.20)

#
# ra8-firmware host-compiled unit test build.
#
# Compiles every library source file with the host (x86_64) compiler
# so clang-tidy has a valid compile_commands.json and so actual unit
# tests can link against mock peripheral implementations.
#
# No target cross-compilation happens here -- that lives in the
# top-level CMakeLists.txt.
#
# Usage from the firmware root:
#
#     cmake -B build/tidy -S tests -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#     cmake --build build/tidy
#
# Or via:
#
#     bash scripts/checks/clang_tidy.sh --check
#

project(
  ra8-firmware-tests
  LANGUAGES C
  VERSION 0.1.0
  DESCRIPTION "Host-compiled unit tests for ra8-firmware"
)


# ---------------------------------------------------------------------------
# Layout (#359)
# ---------------------------------------------------------------------------
# This listfile was 1475 lines. It is now a driver: the build is assembled from
# fragments under tests/cmake/, each one answering a single question.
#
# The include ORDER is load-bearing and is the reason this list is explicit
# rather than a glob:
#
#   host_config      standards, warnings, instrumentation -- must precede any
#                    target, because add_compile_options() only reaches targets
#                    defined after it
#   library_sources  the globs core_hal consumes
#   core_hal         the object library every test links
#   unit_tests       ra8_add_test() and the auto-glob, which needs core_hal
#   tests_*          the targets that cannot come from the auto-glob, each
#                    grouped by WHY: a real crypto backend, a device gate,
#                    C++ XML shims, or a standalone storage subset
#
# A fragment that stopped being included would take its targets with it and
# the build would still succeed, so this list is the one place that decides
# what the host test build contains.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/host_config.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/library_sources.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/core_hal.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/unit_tests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests_crypto.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests_npu.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests_xml.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests_storage.cmake)


# ---------------------------------------------------------------------------
# Optional libFuzzer harnesses (opt-in via -DRA8_FUZZ=ON; clang-only).
# Each fuzz target is built as a separate executable that links against
# the existing ra8_core_hal OBJECT library and adds
# -fsanitize=fuzzer,address,undefined only on its own translation unit.
# Default OFF so the normal host test build is unaffected.
# See docs/FUZZING.md. The RA8_FUZZ option and the slim ra8_core_hal_fuzz
# object library it gates are declared earlier, next to ra8_core_hal.
# ---------------------------------------------------------------------------
add_subdirectory(fuzz)

# ---------------------------------------------------------------------------
# Optional host-side performance benchmarks (opt-in via -DRA8_BENCH=ON).
# Each bench target prints CSV results to stdout. Decoupled from the
# fast unit-test build because the bench binaries build at -O2 and we
# do not want them entangled with coverage/MC-DC instrumentation.
# See docs/PERFORMANCE.md.
# ---------------------------------------------------------------------------
option(RA8_BENCH "Build host-side performance microbenchmarks" OFF)
add_subdirectory(bench)
