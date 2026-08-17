# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Host build configuration: language standards, warning profile, and the
# three mutually-exclusive instrumentation modes (coverage, MC/DC, sanitizer).
#
# Kept together because they interact: RA8_MCDC overrides RA8_COVERAGE, ccache
# opts out when either is on, and the sanitizer picks the image pin that keeps
# .bss clear of the MAP_FIXED peripheral windows. Splitting them further would
# separate a constraint from the thing it constrains.
#
# Included from tests/CMakeLists.txt. CMake include() is textual within the
# same directory scope, so every variable and target defined here is visible
# to the driver and to the fragments included after it.

# Host C23 build -- the target uses the same standard.
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
# Force the C++ standard into every C++ TU's compile command. Without this,
# CMake omits -std when clang's default already satisfies cxx_std_17, leaving
# the .cpp shims (ra8_rabook_xml_shim, ra8_epub_xml_shim, ...) with no -std in
# compile_commands.json -- then clang_tidy.sh's `--extra-arg-before=-std=c2x`
# is the only -std and clang rejects "-std=c2x not allowed with C++".
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ra8_warnings.cmake)

# Every TU is compiled with RA8_OFF_TARGET so code that normally
# reaches into MCU register blocks takes the host-safe path instead.
add_compile_definitions(RA8_OFF_TARGET UNIT_TEST)

# Host warnings are applied per target below. We deliberately do NOT use
# -Wstack-usage=2200 here because the host compiler ABI pushes wider
# arguments than Cortex-M85.

# Coverage instrumentation -- gcovr / lcov friendly.
option(RA8_COVERAGE "Emit gcov/lcov coverage data" ON)
option(RA8_MCDC "Emit clang source-based coverage with MC/DC (DO-178C Level B)" OFF)

# RA8_MCDC takes precedence over RA8_COVERAGE: the two instrumentation
# schemes are mutually exclusive (clang's source-based coverage and
# gcov-style arc/line counters use different runtime libraries).
#
# DO-178C Level B requires Modified Condition/Decision Coverage. The
# only open-source path is clang >= 18 with -fcoverage-mcdc paired
# with -fcoverage-mapping + -fprofile-instr-generate, then reported
# via `llvm-cov show --show-mcdc-summary`.
#
# We probe the host compiler at configure time. If MC/DC is available
# we wire it in for first-party sources only (third_party/ is opted
# out -- see scripts/report/mcdc_report.sh for the SOUP exemption
# policy). If MC/DC is unavailable we fall back to gcc 14's
# `-fcondition-coverage` (decision/condition coverage, NOT MC/DC) and
# print a loud warning so callers know they are NOT measuring MC/DC.
if(RA8_MCDC)
  set(RA8_COVERAGE
      OFF
      CACHE BOOL "Disabled because RA8_MCDC=ON" FORCE
  )

  # Probe: does the active C compiler accept the MC/DC flag trio?
  #
  # Drop any cached answer first (#346). check_c_compiler_flag stores its
  # result in the CACHE and skips the check entirely when that entry already
  # exists -- but the answer is a property of the COMPILER, not of the build
  # dir. Re-configuring an existing tree with a different compiler therefore
  # reuses the previous compiler's verdict: tests/build-cov configured by the
  # coverage gate with gcc-13 (no -fcoverage-mcdc) cached "no", and the mcdc
  # gate's later clang-18 configure silently inherited it, built with plain
  # --coverage, and emitted no .profraw at all while reporting MC/DC support.
  # Re-probing costs about a second; trusting a stale answer cost a gate that
  # could not measure the thing it exists to measure.
  unset(RA8_HAVE_CLANG_MCDC CACHE)
  unset(RA8_HAVE_GCC_CONDCOV CACHE)
  include(CheckCCompilerFlag)
  set(CMAKE_REQUIRED_FLAGS "-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc")
  check_c_compiler_flag("-fcoverage-mcdc" RA8_HAVE_CLANG_MCDC)
  unset(CMAKE_REQUIRED_FLAGS)

  if(RA8_HAVE_CLANG_MCDC)
    message(STATUS "RA8_MCDC: clang MC/DC instrumentation ENABLED "
                   "(-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc)"
    )
    set(RA8_MCDC_C_FLAGS
        -O0
        -g
        -fprofile-instr-generate
        -fcoverage-mapping
        -fcoverage-mcdc
    )
    add_link_options(-fprofile-instr-generate -fcoverage-mapping)
  else()
    # gcc 14+ ships -fcondition-coverage. Probe for it as a best-effort
    # fallback. This is NOT MC/DC -- it is condition coverage only.
    set(CMAKE_REQUIRED_FLAGS "-fprofile-arcs -ftest-coverage -fcondition-coverage")
    check_c_compiler_flag("-fcondition-coverage" RA8_HAVE_GCC_CONDCOV)
    unset(CMAKE_REQUIRED_FLAGS)

    # Degrading here is what turned a one-line configure problem into a
    # misleading failure four steps downstream (#346): the build came out
    # uninstrumented, every test passed, and the gate then blamed the tests
    # for "crashing" because no .profraw existed. RA8_MCDC=ON is an explicit
    # request to measure MC/DC; a toolchain that cannot must say so HERE.
    #
    # RA8_MCDC_ALLOW_FALLBACK=ON opts into the non-MC/DC fallback for local
    # exploration on a gcc-only box. It is deliberately not the default and
    # must never be set on a gate path -- the resulting report contains no
    # condition-level data and cannot support a DO-178C Level B claim.
    if(NOT RA8_MCDC_ALLOW_FALLBACK)
      message(
        FATAL_ERROR
          "RA8_MCDC=ON but ${CMAKE_C_COMPILER} does not support clang's "
          "-fcoverage-mcdc. DO-178C Level B MC/DC CANNOT be measured with "
          "this toolchain, so this configure fails rather than silently "
          "producing an uninstrumented build that yields no .profraw. "
          "Install clang >= 18 (brew install llvm / apt install clang-18), "
          "or pass -DRA8_MCDC_ALLOW_FALLBACK=ON to accept plain "
          "decision/condition coverage instead (NOT MC/DC)."
      )
    endif()

    message(
      WARNING "RA8_MCDC requested but the host compiler does not support "
              "clang's -fcoverage-mcdc. DO-178C Level B MC/DC CANNOT be "
              "measured with this toolchain. "
              "Install clang >= 18 (brew install llvm / apt install clang-18) "
              "and re-run."
    )

    if(RA8_HAVE_GCC_CONDCOV)
      message(WARNING "Falling back to gcc -fcondition-coverage (condition coverage "
                      "only, NOT MC/DC)."
      )
      set(RA8_MCDC_C_FLAGS
          -O0
          -g
          --coverage
          -fprofile-arcs
          -ftest-coverage
          -fcondition-coverage
      )
      add_link_options(--coverage -fprofile-arcs -ftest-coverage)
    else()
      message(WARNING "Falling back to plain gcov decision/branch coverage. No "
                      "condition-level data will be produced."
      )
      set(RA8_MCDC_C_FLAGS
          -O0
          -g
          --coverage
          -fprofile-arcs
          -ftest-coverage
      )
      add_link_options(--coverage -fprofile-arcs -ftest-coverage)
    endif()
  endif()

  # Apply globally so test executables get the same flags as the
  # ra8_core_hal OBJECT library. Per-source opt-out for third_party
  # is performed below, after the source lists are populated.
  add_compile_options(${RA8_MCDC_C_FLAGS})
elseif(RA8_COVERAGE)
  # A target may opt out of gcov instrumentation by setting the custom property
  # RA8_SKIP_COVERAGE_INSTRUMENTATION. Exactly one does: test_ra8_unity_output
  # forges source names with #line directives so it can pin Unity's rendered
  # "[FAIL] <file>:<line> <msg>" diagnostic byte for byte. gcov records those
  # synthetic names in its notes; gcovr then cannot resolve `unity_output_fixture.c`
  # from ANY directory, walks up to `/`, and aborts the whole report with
  # `no_working_dir_found` (exit 64) -- no XML, so the gate fails having measured
  # nothing.
  #
  # This has to be a generator expression on the DIRECTORY options, which is
  # where the coverage flags live. Appending `-fno-profile-arcs
  # -fno-test-coverage` to the target instead does NOT work: measured with
  # gcc-14, `--coverage -fno-profile-arcs -fno-test-coverage` still emits the
  # .gcno and still leaves __gcov0.* counters in the object, because the driver
  # expands --coverage independently of the later negation. It is also a hard
  # error under clang (`argument unused during compilation` with -Werror), which
  # is what broke the scan-build gate.
  set(_ra8_cov_skip "$<TARGET_PROPERTY:RA8_SKIP_COVERAGE_INSTRUMENTATION>")
  add_compile_options(
    -O0 -g "$<$<NOT:$<BOOL:${_ra8_cov_skip}>>:--coverage;-fprofile-arcs;-ftest-coverage>"
  )
  add_link_options(--coverage -fprofile-arcs -ftest-coverage)
endif()

# Compiler cache. Included AFTER the coverage / MC-DC options are resolved
# above, because it opts out when either is on -- gcov and clang profile data
# embed absolute paths that a cached object would carry into the wrong build
# directory. See cmake/ccache.cmake for the full rationale.
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ccache.cmake")

# GNU ld's `-Ttext-segment` is an ELF/Linux image-layout control. Darwin's
# linker does not implement it, and macOS does not map the Linux peripheral
# windows that require the host-test image pin in the first place.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(RA8_HOST_IMAGE_PIN_OPTIONS -no-pie -Wl,-Ttext-segment=0x70000000)
endif()

# RA8_SANITIZE: when non-empty (e.g. "undefined"), instrument every host-test
# target with -fsanitize=<list> -- the undefined-behaviour gate driven by
# `make ubsan`. Independent of the coverage / MC-DC instrumentation above; pick
# at most one of {coverage, mcdc, sanitize} per build tree (separate dirs).
set(RA8_SANITIZE
    ""
    CACHE STRING "Comma-separated -fsanitize list for host tests (e.g. undefined)"
)
if(RA8_SANITIZE)
  add_compile_options(-fsanitize=${RA8_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${RA8_SANITIZE})
  # UBSan reserves no low-memory shadow (unlike ASan), so it can still take the
  # image pin below that keeps text/.data/.bss/brk clear of the MAP_FIXED
  # peripheral windows. Without it a ~40 MiB test image -- the 32 MiB XSPI fake
  # backing bundled into ra8_core_hal dominates every test's .bss -- loads at
  # 0x400000 and its .bss crosses the code-MRAM window at 0x02000000, so
  # ra8_fake_mmap_install aborts before main(). Apply the pin for every sanitizer
  # except address, which owns its own low-address layout and skips the
  # shadow-gap window at runtime instead.
  if(RA8_HOST_IMAGE_PIN_OPTIONS AND NOT RA8_SANITIZE MATCHES "address")
    add_link_options(${RA8_HOST_IMAGE_PIN_OPTIONS})
  endif()
else()
  # The MAP_FIXED peripheral windows span [0x02000000, 0x68100000) (see
  # tests/mocks/ra8_fake_mmap.c). On toolchains that default to non-PIE the
  # image loads at 0x400000, so a large-enough .bss (the gcov counters of a
  # coverage build crossed 0x02000000 in practice) pushes static data and the
  # brk heap INTO a window; the constructor's mmap then silently replaces live
  # pages and glibc aborts on the first heap extension. Pin the image above
  # every window so text/.data/.bss/brk can never intersect one. AddressSanitizer
  # is excluded above: it owns its low layout and skips the shadow-gap window
  # instead (ra8_fake_mmap_install still checks overlap at runtime).
  if(RA8_HOST_IMAGE_PIN_OPTIONS)
    add_link_options(${RA8_HOST_IMAGE_PIN_OPTIONS})
  endif()
endif()
