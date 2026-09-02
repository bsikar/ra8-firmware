# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# Toolchain file for Renesas RA8P1 (R7KA8P1KFLCAC: Arm Cortex-M85 + Cortex-M33
# + Arm Ethos-U55 NPU) with the ARM GNU Toolchain.
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8p1.cmake ..
#
# -----------------------------------------------------------------------------
# Why this file layers on top of toolchain-ra8d2.cmake instead of duplicating it
# -----------------------------------------------------------------------------
# The RA8P1 and RA8D2 are the same RA8 family. Both are an Arm Cortex-M85
# primary core (FPv5 single-precision FPU, Helium / MVE, TrustZone-M, PACBTI)
# plus an Arm Cortex-M33 secondary core, in a pin-compatible 289-pin BGA
# (R7KA8P1KFLCAC vs R7KA8D2KFLCAC). See the difference analysis issue for the
# sourced comparison; RA8P1 datasheet is Renesas R01DS0439EJ, RA8D2 is
# R01DS0493EJ.
#
# Because the two parts share the core, the cross-compiler selection, the pinned
# Arm GNU Toolchain version gate, the -mcpu / -mfloat-abi CPU flags, and the
# newlib-nano linker flags are all IDENTICAL. Rather than copy that body (and
# risk it drifting between the two chips), this file includes the RA8D2
# toolchain verbatim and then adds only what genuinely differs at compile time:
#
#   1. the device-selection define -DRA8_DEVICE_RA8P1 (see below), and
#   2. the FPU width: -mfpu=fpv5-d16 (double-precision) -- see the FPU section.
#
# libs/ra8_core/inc/ra8_device.h reads the device define to switch register bases,
# memory-map sizes, and feature flags to the RA8P1. The RA8D2 build passes NO
# device define and no -mfpu override, so ra8_device.h defaults to RA8D2 and the
# RA8D2 firmware is byte-for-behaviour unchanged by this addition (378 per-app
# justfiles hardcode cmake/toolchain-ra8d2.cmake by name -- it must keep working
# unchanged, so it is deliberately left untouched).
#
# -----------------------------------------------------------------------------
# FPU precision -- RA8P1 has a DOUBLE-precision FPU (issue #225)
# -----------------------------------------------------------------------------
# The RA8D2 primary M85 is single-precision (fpv5-sp-d16, __FPU_DP == 0); the
# RA8P1 primary M85 exposes the DOUBLE-precision FPU, so this file overrides
# -mfpu to fpv5-d16 while keeping -mfloat-abi=hard. That lets `double` code run
# on the FPU as hardware .f64 opcodes (vmul.f64 / vadd.f64) instead of the
# soft-float library calls the SP build emits. [CONFIRM: RA8P1 M85 __FPU_DP == 1
# / DP-FPU presence to be re-verified against the RA8P1 CMSIS core header and
# HUM R01UH1064EJ once silicon/datasheet access is available.]
#
# HOW THE OVERRIDE WORKS: the included toolchain-ra8d2.cmake bakes
# -mfpu=fpv5-sp-d16 into CMAKE_{C,CXX,ASM,EXE_LINKER}_FLAGS_INIT. GCC honours the
# LAST -mfpu on the command line, so appending -mfpu=fpv5-d16 after the include
# wins for compile AND link (the link step selects the newlib-nano multilib from
# the effective -mfpu, so the linker flags must carry the override too). The
# RA8D2 toolchain file itself is never edited, so the RA8D2 codegen is untouched.
#
# -----------------------------------------------------------------------------
# If the two chips ever need genuinely more divergent compile flags
# -----------------------------------------------------------------------------
# Split the shared body of toolchain-ra8d2.cmake into cmake/toolchain-ra8-
# common.cmake and make both toolchain-ra8d2.cmake and this file thin wrappers.
# Today only -mfpu and the device define differ, so the append keeps churn low.

include(${CMAKE_CURRENT_LIST_DIR}/toolchain-ra8d2.cmake)

# DP-FPU override (see the FPU section above). fpv5-d16 == FPv5 double-precision,
# 16 D-registers. Appended last so it beats the fpv5-sp-d16 the RA8D2 file set,
# on every flag group that feeds an -mfpu-sensitive step.
set(RA8P1_FPU_FLAG "-mfpu=fpv5-d16")
set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} ${RA8P1_FPU_FLAG}")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} ${RA8P1_FPU_FLAG}")
set(CMAKE_ASM_FLAGS_INIT "${CMAKE_ASM_FLAGS_INIT} ${RA8P1_FPU_FLAG}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT} ${RA8P1_FPU_FLAG}")

# Device-selection define -- consumed by libs/ra8_core/inc/ra8_device.h to pick
# the RA8P1 register bases / memory sizes / feature set. Appended to the *_INIT
# flags (read once at the first project()) exactly the way the CPU flags are
# injected inside toolchain-ra8d2.cmake. This is idempotent across CMake's
# repeated toolchain includes: the included RA8D2 file re-sets each *_INIT var to
# its device-agnostic base first, then this re-appends the define.
set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -DRA8_DEVICE_RA8P1")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -DRA8_DEVICE_RA8P1")
set(CMAKE_ASM_FLAGS_INIT "${CMAKE_ASM_FLAGS_INIT} -DRA8_DEVICE_RA8P1")

message(STATUS "toolchain-ra8p1: RA8P1 (R7KA8P1KFLCAC) selected -- RA8_DEVICE_RA8P1")
