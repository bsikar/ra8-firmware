# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# tools/ra8_emulator/cmake/ra8_emu_require_c23.cmake
#
# Preflight the selected C compiler before anything else in this project is
# configured, so a standalone `cmake -B build -S .` on a box whose ambient `cc`
# is too old fails here, by name, instead of deep in a compile of
# `src/main.c` with diagnostics that read like defects in that file.
#
# The emulator is written in C23: fixed-underlying-type enums
# (`typedef enum : uint8_t`) and `nullptr`. Those need gcc >= 13 or
# clang >= 17. CMake defaults to a bare `cc`, which on the Debian 12 dev box is
# gcc 12 and rejects the syntax outright -- and GCC 12's parse failure surfaces
# as unused-variable and return-type warnings in valid code, which is how the
# real cause stayed hidden.
#
# The repository's own builders never reach this check in anger: every one of
# them (`scripts/builders/build_host_tools.sh`, `scripts/emu/smoke.sh`,
# `scripts/emu/matrix.sh`, `scripts/emu/eil_all.sh`) pins a probed pair through
# `ra8_select_emulator_compiler` in `scripts/builders/select_host_compiler.sh`
# and passes it as `-DCMAKE_C_COMPILER`, so the probe below simply passes. CI
# behaviour is therefore unchanged; what changes is the diagnostic a developer
# gets when they configure the project by hand.
#
# The test is a compile of the syntax rather than a version-string comparison,
# matching how `ra8_c23_compiler_ok` in that shell helper decides the same
# question, so the two cannot disagree and neither needs updating when a new
# compiler release lands.
#

include_guard(GLOBAL)

# Fail the configure unless the selected C compiler parses the C23 constructs
# this tool is written in. Call once, after the C language is enabled and after
# CMAKE_C_STANDARD is set, and before any target is defined.
function(ra8_emu_require_c23)
  include(CheckCSourceCompiles)

  # -std=gnu2x, not -std=c23: gcc 13.x spells the standard that way, and gcc 12
  # accepts the flag too, so the probe fails on the typed enum -- the thing
  # actually being tested -- rather than on an unrecognised option.
  set(CMAKE_REQUIRED_FLAGS "-std=gnu2x")
  check_c_source_compiles(
    "typedef enum : int { k_probe = 0 } probe_t;
     int main(void) { int* p = nullptr; (void)p; return (int)k_probe; }"
    RA8_EMU_C_COMPILER_IS_C23
  )

  if(NOT RA8_EMU_C_COMPILER_IS_C23)
    message(
      FATAL_ERROR
      "ra8_emulator needs a C23 compiler and '${CMAKE_C_COMPILER}' is not one.\n"
      "It could not compile a fixed-underlying-type enum plus nullptr, which "
      "this tool's sources use throughout. Required: gcc >= 13 or clang >= 17.\n"
      "\n"
      "Either build it the way the repository does, from the repo root:\n"
      "    just tools::build_one ra8_emulator\n"
      "which selects and probes a supported pair for you, or name one here:\n"
      "    cmake -B build -S . -DCMAKE_C_COMPILER=gcc-13\n"
      "\n"
      "Do not work around this by editing the sources: the C23 they use is "
      "deliberate, and a compiler that cannot parse it reports the failure as "
      "unrelated warnings in valid code."
    )
  endif()
endfunction()
