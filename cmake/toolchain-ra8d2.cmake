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

# Find the toolchain binaries
find_program(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}gcc     REQUIRED)
find_program(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++     REQUIRED)
find_program(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc     REQUIRED)
find_program(CMAKE_OBJCOPY      ${TOOLCHAIN_PREFIX}objcopy REQUIRED)
find_program(CMAKE_OBJDUMP      ${TOOLCHAIN_PREFIX}objdump REQUIRED)
find_program(CMAKE_SIZE         ${TOOLCHAIN_PREFIX}size    REQUIRED)
find_program(CMAKE_AR           ${TOOLCHAIN_PREFIX}ar      REQUIRED)
find_program(CMAKE_RANLIB       ${TOOLCHAIN_PREFIX}ranlib  REQUIRED)

# Don't try to run the compiler on the host to test it -- it can't produce
# host executables because it targets Cortex-M85.
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)

# -----------------------------------------------------------------------------
# Pin the cross-compiler major version (#178)
# -----------------------------------------------------------------------------
# Codegen correctness on the attacker-facing miniz ZIP inflater is
# version-specific: Arm GNU Toolchain 13.3 miscompiles it under strict aliasing
# where other majors do not (worked around with -fno-strict-aliasing on the SOUP
# TUs -- see cmake/ra_add_app.cmake). The release binary is built by the CI
# build-cross job with Arm GNU Toolchain 13.x, so a stray host toolchain must not
# silently change the shipped codegen. `find_program`/CMAKE_C_COMPILER_WORKS give
# no version assertion, so add one here.
#
# Default: WARN on a mismatch (so a developer on a different major can still build
# and test locally). CI's build-cross sets -DRA_STRICT_TOOLCHAIN=ON to make the
# mismatch FATAL, guaranteeing the release path uses the pinned major.
# See docs/TOOLCHAIN.md.
set(RA_PINNED_ARM_GCC_MAJOR 13 CACHE STRING "Pinned arm-none-eabi-gcc major version (#178)")
option(RA_STRICT_TOOLCHAIN "Fail (not warn) when arm-none-eabi-gcc is not the pinned major" OFF)
# Let CI enforce the pin without threading -D through the per-app Makefiles:
# `RA_STRICT_TOOLCHAIN=1` in the environment promotes the warning to an error.
if(DEFINED ENV{RA_STRICT_TOOLCHAIN} AND NOT "$ENV{RA_STRICT_TOOLCHAIN}" STREQUAL "0")
  set(RA_STRICT_TOOLCHAIN ON)
endif()
execute_process(
  COMMAND ${CMAKE_C_COMPILER} -dumpversion
  OUTPUT_VARIABLE _ra_arm_gcc_version
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _ra_arm_gcc_rc)
if(NOT _ra_arm_gcc_rc EQUAL 0)
  message(FATAL_ERROR "toolchain-ra8d2: '${CMAKE_C_COMPILER} -dumpversion' failed (rc=${_ra_arm_gcc_rc})")
endif()
string(REGEX MATCH "^[0-9]+" _ra_arm_gcc_major "${_ra_arm_gcc_version}")
if(NOT _ra_arm_gcc_major STREQUAL "${RA_PINNED_ARM_GCC_MAJOR}")
  set(_ra_tc_msg
    "arm-none-eabi-gcc is ${_ra_arm_gcc_version}, but this project pins Arm GNU "
    "Toolchain ${RA_PINNED_ARM_GCC_MAJOR}.x (13.3) for reproducible codegen -- the "
    "miniz inflater is version-sensitive (#178). Install 13.3 (see "
    "docs/TOOLCHAIN.md) for a byte-reproducible build.")
  if(RA_STRICT_TOOLCHAIN)
    message(FATAL_ERROR ${_ra_tc_msg})
  else()
    message(WARNING ${_ra_tc_msg})
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
set(RA8D2_CPU_FLAGS
    "-mcpu=cortex-m85"
    "-mthumb"
    "-mfloat-abi=hard"
    "-mfpu=fpv5-sp-d16"
)
string(JOIN " " RA8D2_CPU_FLAGS_STR ${RA8D2_CPU_FLAGS})

# Common compiler flags (applied in addition to target-specific flags in the
# top-level CMakeLists.txt).
set(CMAKE_C_FLAGS_INIT   "${RA8D2_CPU_FLAGS_STR} -fdata-sections -ffunction-sections")
set(CMAKE_CXX_FLAGS_INIT "${RA8D2_CPU_FLAGS_STR} -fdata-sections -ffunction-sections -fno-rtti -fno-exceptions")
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
     -Wl,--print-memory-usage")

# Cross-compile search paths: look in the target sysroot, not the host
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
