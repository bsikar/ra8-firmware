# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# Toolchain file for Renesas RA8D2 (Arm Cortex-M85) with ARM GNU Toolchain
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8d2.cmake ..
#
# Install ARM GNU Toolchain from:
#   https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
# and ensure arm-none-eabi-gcc is in PATH (or edit TOOLCHAIN_PREFIX below).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Cross-compiler prefix
set(TOOLCHAIN_PREFIX arm-none-eabi-)

# Prefer the pinned Arm GNU Toolchain 13.3 install if present, at the per-OS
# standard path, so every environment (Mac, dev box, CI runner, devcontainer)
# builds with the SAME compiler regardless of what stray arm-gcc sits on PATH
# (#178). find_program searches HINTS before the system PATH, so a 13.3 install
# here wins; absent it, the search falls back to PATH and the version assertion
# below catches a mismatch. To relocate, install to one of these paths (see
# docs/TOOLCHAIN.md) or pass -DRA8_ARM_TOOLCHAIN_BIN=<dir>.
set(_ra8_pinned_tc_bin ${RA8_ARM_TOOLCHAIN_BIN} /opt/arm-gnu-toolchain-13.3/bin
                       $ENV{HOME}/opt/arm-gnu-toolchain-13.3/bin
)

# Find the toolchain binaries
find_program(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}gcc HINTS ${_ra8_pinned_tc_bin} REQUIRED)
find_program(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++ HINTS ${_ra8_pinned_tc_bin} REQUIRED)
find_program(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc HINTS ${_ra8_pinned_tc_bin} REQUIRED)
find_program(CMAKE_OBJCOPY ${TOOLCHAIN_PREFIX}objcopy HINTS ${_ra8_pinned_tc_bin} REQUIRED)
find_program(CMAKE_OBJDUMP ${TOOLCHAIN_PREFIX}objdump HINTS ${_ra8_pinned_tc_bin} REQUIRED)
find_program(CMAKE_SIZE ${TOOLCHAIN_PREFIX}size HINTS ${_ra8_pinned_tc_bin} REQUIRED)
find_program(CMAKE_AR ${TOOLCHAIN_PREFIX}ar HINTS ${_ra8_pinned_tc_bin} REQUIRED)
find_program(CMAKE_RANLIB ${TOOLCHAIN_PREFIX}ranlib HINTS ${_ra8_pinned_tc_bin} REQUIRED)
# CMAKE_NM is consumed by the TrustZone app's post-build SG-veneer offset gate
# (scripts/checks/check_sg_offsets.py) so it reads the SAME cross nm as the link.
find_program(CMAKE_NM ${TOOLCHAIN_PREFIX}nm HINTS ${_ra8_pinned_tc_bin} REQUIRED)

# Don't try to run the compiler on the host to test it -- it can't produce
# host executables because it targets Cortex-M85.
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)

# -----------------------------------------------------------------------------
# Compiler cache for EVERY cross build
# -----------------------------------------------------------------------------
# This has to be here and not only in the top-level CMakeLists. Apps are built
# standalone -- `make <app>` configures examples/<app>/ with this toolchain file
# and never processes the root listfile -- which is exactly how
# build_all_examples.sh and the CI build-cross job build all ~200 of them.
# Including cmake/ccache.cmake from the root alone therefore left the largest
# and most repetitive compile workload in the project with no cache at all: a
# full cross build was observed running with the ccache counters frozen.
#
# The toolchain file is the one place every cross build passes through,
# whatever the entry point. toolchain-ra8p1.cmake includes this file verbatim,
# so the RA8P1 cross builds are covered by the same line.
include("${CMAKE_CURRENT_LIST_DIR}/ccache.cmake")

# -----------------------------------------------------------------------------
# Pin the cross-compiler major version (#178)
# -----------------------------------------------------------------------------
# Codegen correctness on the attacker-facing miniz ZIP inflater is
# version-specific: Arm GNU Toolchain 13.3 miscompiles it under strict aliasing
# where other majors do not (worked around with -fno-strict-aliasing on the SOUP
# TUs -- see cmake/ra8_add_app.cmake). The release binary is built by the CI
# build-cross job with Arm GNU Toolchain 13.x, so a stray host toolchain must not
# silently change the shipped codegen. `find_program`/CMAKE_C_COMPILER_WORKS give
# no version assertion, so add one here.
#
# Default: FATAL on a mismatch. Every environment installs the pinned 13.3 at a
# standard path (found via the HINTS above), so the single-version convergence is
# enforced everywhere (#178): a build that silently picks up a stray arm-gcc fails
# loudly instead of shipping version-divergent codegen. For a deliberate one-off
# local build on a different toolchain, pass -DRA8_STRICT_TOOLCHAIN=OFF (or
# RA8_STRICT_TOOLCHAIN=0 in the environment) to downgrade the mismatch to a
# warning. See docs/TOOLCHAIN.md.
set(RA8_PINNED_ARM_GCC_VERSION
    "13.3"
    CACHE STRING "Pinned arm-none-eabi-gcc major.minor (13.3.rel1) (#178)"
)
option(RA8_STRICT_TOOLCHAIN "Fail (not warn) when arm-none-eabi-gcc is not the pinned version" ON)
# Escape hatch: RA8_STRICT_TOOLCHAIN=0 in the environment downgrades to a warning.
if(DEFINED ENV{RA8_STRICT_TOOLCHAIN} AND "$ENV{RA8_STRICT_TOOLCHAIN}" STREQUAL "0")
  set(RA8_STRICT_TOOLCHAIN OFF)
endif()
execute_process(
  COMMAND ${CMAKE_C_COMPILER} -dumpfullversion
  OUTPUT_VARIABLE _ra8_arm_gcc_version
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _ra8_arm_gcc_rc
)
if(NOT _ra8_arm_gcc_rc EQUAL 0)
  message(
    FATAL_ERROR
      "toolchain-ra8d2: '${CMAKE_C_COMPILER} -dumpfullversion' failed (rc=${_ra8_arm_gcc_rc})"
  )
endif()
# Pin to major.minor -- the Arm GNU Toolchain release line (13.3.rel1 ships gcc
# 13.3.1). Patch-tolerant so a 13.3.x point release still matches, but 13.2 / 14.x
# do not. This is stricter than the previous major-only check (which accepted the
# codegen-divergent 13.2).
string(REGEX MATCH "^[0-9]+\\.[0-9]+" _ra8_arm_gcc_mm "${_ra8_arm_gcc_version}")
if(NOT _ra8_arm_gcc_mm STREQUAL "${RA8_PINNED_ARM_GCC_VERSION}")
  set(_ra8_tc_msg
      "arm-none-eabi-gcc is ${_ra8_arm_gcc_version} (${CMAKE_C_COMPILER}), but this "
      "project pins Arm GNU Toolchain ${RA8_PINNED_ARM_GCC_VERSION} (13.3.rel1) for "
      "reproducible codegen -- the miniz inflater is version-sensitive (#178). "
      "Install 13.3.rel1 to /opt/arm-gnu-toolchain-13.3 or "
      "~/opt/arm-gnu-toolchain-13.3 (see docs/TOOLCHAIN.md), or pass "
      "-DRA8_STRICT_TOOLCHAIN=OFF for a deliberate one-off build."
  )
  if(RA8_STRICT_TOOLCHAIN)
    message(FATAL_ERROR ${_ra8_tc_msg})
  else()
    message(WARNING ${_ra8_tc_msg})
  endif()
endif()

# -----------------------------------------------------------------------------
# RA8D2 (Cortex-M85) CPU flags
# -----------------------------------------------------------------------------
# The RA8D2 primary core is a Cortex-M85 with:
#   - FPv5 SINGLE-precision FPU only (per R7KA8D2KF_core0.h: __FPU_DP = 0)
#   - Helium / M-profile Vector Extension (MVE) integer + FP
#   - TrustZone-M and PACBTI (not used yet)
#
# Hard-float ABI because we have an FPU.
# -mthumb because the M-profile cores are Thumb-only.
# fpv5-sp-d16 is the single-precision variant; fpv5-d16 (double-precision)
# would silently generate instructions the RA8D2 cannot execute.
# -----------------------------------------------------------------------------
set(RA8D2_CPU_FLAGS "-mcpu=cortex-m85" "-mthumb" "-mfloat-abi=hard" "-mfpu=fpv5-sp-d16")
string(JOIN " " RA8D2_CPU_FLAGS_STR ${RA8D2_CPU_FLAGS})

# Common compiler flags (applied in addition to target-specific flags in the
# top-level CMakeLists.txt).
set(CMAKE_C_FLAGS_INIT "${RA8D2_CPU_FLAGS_STR} -fdata-sections -ffunction-sections")
set(CMAKE_CXX_FLAGS_INIT
    "${RA8D2_CPU_FLAGS_STR} -fdata-sections -ffunction-sections -fno-rtti -fno-exceptions"
)
set(CMAKE_ASM_FLAGS_INIT "${RA8D2_CPU_FLAGS_STR}")

# Linker flags
#   --specs=nano.specs    use newlib-nano (smaller libc)
#   --specs=nosys.specs   stub out syscalls (no host OS)
#   --gc-sections         drop unused input sections
#   -nostartfiles         we supply our own Reset_Handler and vector table
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "${RA8D2_CPU_FLAGS_STR} \
     --specs=nano.specs \
     --specs=nosys.specs \
     -nostartfiles \
     -Wl,--gc-sections \
     -Wl,--print-memory-usage"
)

# Cross-compile search paths: look in the target sysroot, not the host
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
