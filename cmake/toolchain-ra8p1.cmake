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
# Arm GNU Toolchain 13.3 version gate, the -mcpu / -mfpu / -mfloat-abi CPU flags,
# and the newlib-nano linker flags are all IDENTICAL. Rather than copy that body
# (and risk it drifting between the two chips), this file includes the RA8D2
# toolchain verbatim and then adds the ONE thing that differs at compile time:
# the device-selection define -DRA_DEVICE_RA8P1. libs/ra_core/inc/ra_device.h
# reads that define to switch register bases, memory-map sizes, and feature
# flags to the RA8P1. The RA8D2 build passes NO device define and ra_device.h
# defaults to RA8D2, so the RA8D2 firmware is byte-for-behaviour unchanged by
# this addition (378 per-app Makefiles hardcode cmake/toolchain-ra8d2.cmake by
# name -- it must keep working unchanged, so it is deliberately left untouched).
#
# -----------------------------------------------------------------------------
# FPU precision
# -----------------------------------------------------------------------------
# Both cores are configured single-precision (fpv5-sp-d16), matching RA8D2.
# -mfpu=fpv5-sp-d16 is correctness-safe on any Cortex-M85: single-precision code
# executes on a double-precision FPU as well, so even if a future RA8P1 SKU were
# to expose a double-precision FPU this flag stays correct -- it would only
# leave DP throughput unused, which is a deliberate performance follow-up, never
# a miscompile.
#
# -----------------------------------------------------------------------------
# If the two chips ever need genuinely different compile flags
# -----------------------------------------------------------------------------
# Split the shared body of toolchain-ra8d2.cmake into cmake/toolchain-ra8-
# common.cmake and make both toolchain-ra8d2.cmake and this file thin wrappers.
# Today the flags are identical, so the include keeps churn at zero.

include(${CMAKE_CURRENT_LIST_DIR}/toolchain-ra8d2.cmake)

# Device-selection define -- consumed by libs/ra_core/inc/ra_device.h to pick
# the RA8P1 register bases / memory sizes / feature set. Appended to the *_INIT
# flags (read once at the first project()) exactly the way the CPU flags are
# injected inside toolchain-ra8d2.cmake. This is idempotent across CMake's
# repeated toolchain includes: the included RA8D2 file re-sets each *_INIT var to
# its device-agnostic base first, then this re-appends the define.
set(CMAKE_C_FLAGS_INIT   "${CMAKE_C_FLAGS_INIT} -DRA_DEVICE_RA8P1")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -DRA_DEVICE_RA8P1")
set(CMAKE_ASM_FLAGS_INIT "${CMAKE_ASM_FLAGS_INIT} -DRA_DEVICE_RA8P1")

message(STATUS "toolchain-ra8p1: RA8P1 (R7KA8P1KFLCAC) selected -- RA_DEVICE_RA8P1")
