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
#   2. nothing else by default: the FPU width is NOT a delta between the two
#      parts (issue #225, see the FPU section), so -mfpu is inherited unchanged
#      and fpv5-d16 is reachable only through the opt-in RA8P1_DP_FPU switch.
#
# libs/ra8_core/inc/ra8_device.h reads the device define to switch register bases,
# memory-map sizes, and feature flags to the RA8P1. The RA8D2 build passes NO
# device define and no -mfpu override, so ra8_device.h defaults to RA8D2 and the
# RA8D2 firmware is byte-for-behaviour unchanged by this addition (378 per-app
# justfiles hardcode cmake/toolchain-ra8d2.cmake by name -- it must keep working
# unchanged, so it is deliberately left untouched).
#
# -----------------------------------------------------------------------------
# FPU precision -- SETTLED: the RA8P1 FPU is NOT a delta vs the RA8D2 (issue #225)
# -----------------------------------------------------------------------------
# This file used to override -mfpu to fpv5-d16 unconditionally, on the strength
# of the RA8P1 datasheet's Cortex-M85 FPU line, with a [CONFIRM] marker left in
# place. Both sources have now been read directly and they do NOT support an
# RA8P1-specific double-precision build:
#
#   * FSP CMSIS device header R7KA8P1KF_core0.h (the RA8P1 primary M85) declares
#     __FPU_PRESENT 1 and __FPU_DP 0, byte-identical to R7KA8D2KF_core0.h. The
#     RA8P1 secondary core header (R7KA8P1KF_core1.h, M33) also declares
#     __FPU_DP 0. No RA8 CMSIS header in FSP declares a double-precision FPU.
#   * RA8P1 datasheet R01DS0439EJ0130 Table 1.1 "Function Outline" says of the
#     M85 "Scalar half, single, and double-precision floating-point operation"
#     -- but the RA8D2 datasheet R01DS0493EJ says THE SAME SENTENCE about ITS
#     M85, and the RA8D2 is built single-precision here. The sentence therefore
#     describes the Cortex-M85 r1p1 FPU the family licenses, and distinguishes
#     nothing between the two parts. (Both datasheets do distinguish the
#     secondary M33, which they call single-precision only, so the wording is
#     deliberate per core, not boilerplate per document.)
#
# So the two parts are treated identically: -mfpu=fpv5-sp-d16, inherited from
# toolchain-ra8d2.cmake, which is correctness-safe on both. Selecting fpv5-d16
# for a part whose vendor header declares no DP FPU would emit .f64 opcodes that
# a single-precision FPU takes as UNDEFINED, i.e. a HardFault on first silicon.
#
# THE DP BUILD IS STILL REACHABLE, as an explicit opt-in for the on-silicon
# benchmark issue #229 asks for, and never by default:
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8p1.cmake -DRA8P1_DP_FPU=ON ..
#
# That appends -mfpu=fpv5-d16 (last -mfpu wins, for compile AND link, because the
# link step picks the newlib-nano multilib from the effective -mfpu) and defines
# RA8_FPU_DP_ENABLED so libs/ra8_hal/inc/ra8_fpu_probe.h can cross-check the
# request against what the compiler actually selected. Both multilibs ship in the
# pinned Arm GNU Toolchain 13.3.Rel1: fpv5-d16 resolves to
# thumb/v8-m.main+dp/hard, fpv5-sp-d16 to thumb/v8-m.main+fp/hard.
#
# An opt-in DP image is UNVALIDATED on hardware. Whether this silicon executes
# .f64 at all is exactly what #229 has to measure on an RA8P1 EK; until then the
# opt-in is a bench switch, not a supported configuration.
#
# -----------------------------------------------------------------------------
# If the two chips ever need genuinely more divergent compile flags
# -----------------------------------------------------------------------------
# Split the shared body of toolchain-ra8d2.cmake into cmake/toolchain-ra8-
# common.cmake and make both toolchain-ra8d2.cmake and this file thin wrappers.
# Today only -mfpu and the device define differ, so the append keeps churn low.

include(${CMAKE_CURRENT_LIST_DIR}/toolchain-ra8d2.cmake)

# Opt-in DP-FPU build for the #229 on-silicon benchmark (see the FPU section
# above). OFF by default, so the RA8P1 inherits the RA8D2's fpv5-sp-d16. When ON,
# fpv5-d16 (FPv5 double-precision, 16 D-registers) is appended LAST so it beats
# the inherited flag on every group that feeds an -mfpu-sensitive step, compile
# and link alike, and RA8_FPU_DP_ENABLED is defined so the code can tell it was
# asked for rather than inferring it from codegen.
if(RA8P1_DP_FPU)
  set(RA8P1_FPU_FLAG "-mfpu=fpv5-d16")
  set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} ${RA8P1_FPU_FLAG} -DRA8_FPU_DP_ENABLED")
  set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} ${RA8P1_FPU_FLAG} -DRA8_FPU_DP_ENABLED")
  set(CMAKE_ASM_FLAGS_INIT "${CMAKE_ASM_FLAGS_INIT} ${RA8P1_FPU_FLAG}")
  set(CMAKE_EXE_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT} ${RA8P1_FPU_FLAG}")
  message(STATUS "toolchain-ra8p1: RA8P1_DP_FPU=ON -- fpv5-d16, UNVALIDATED on silicon (#229)")
endif()

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
