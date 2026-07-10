#
# cmake/tflite_micro.cmake
#
# Top-level integration for the vendored TensorFlow Lite for Microcontrollers
# (TFLite-micro) inference runtime on the RA8 family (Cortex-M85). This is the
# on-device inference engine for the RA8P1 Ethos-U55 NPU story: it parses a
# Vela-compiled .tflite model (FlatBuffer), plans the tensor arena, and drives
# the operator graph. The Ethos-U custom operator is the hand-off point to the
# NPU.
#
# The runtime is vendored as SOUP under libs/third_party/ in a LEAN subset:
#
#   * libs/third_party/tflite-micro  -- MicroInterpreter + MicroAllocator + the
#     op resolver + a modest reference-kernel set (CONV_2D, DEPTHWISE_CONV_2D,
#     FULLY_CONNECTED, ADD, MUL, RESHAPE, SOFTMAX, AVERAGE_POOL_2D) + the
#     Ethos-U custom-op stub. Audio/FFT (signal/, kissfft), the CMSIS-NN /
#     Xtensa / ARC optimized kernel ports, and every unused kernel are omitted.
#   * libs/third_party/flatbuffers   -- the FlatBuffer read-path headers the
#     .tflite model format needs (headers only; no compiler/codegen).
#   * libs/third_party/gemmlowp      -- fixed-point math headers the quantized
#     reference kernels depend on.
#   * libs/third_party/ruy           -- the profiler instrumentation stub header
#     the kernel utilities include (no ruy GEMM backend).
#
# Exposes the RA_USE_TFLITE_MICRO option; when ON, this file:
#
#   1. Enables the C++ language (the top-level firmware project is C + ASM only;
#      TFLite-micro is C++17).
#   2. Compiles the vendored *.cc into a single tflite_micro_objs OBJECT library
#      so apps splice the objects directly into their final ELF without dragging
#      the vendored include tree into the rest of the build.
#   3. Publishes a consumer-facing tflite_micro INTERFACE target carrying the
#      include directories and the TF_LITE_STATIC_MEMORY define.
#
# Apps that want inference link against tflite_micro; include dirs and defines
# flow through the interface target.
#
# Phase 2 (NOT wired here): adapt the Ethos-U operator to call the first-party
# libs/ra_hal ra_npu driver (ra_npu_submit / ra_npu_run / ra_npu_wait) instead
# of the Arm ethos-u-core-driver. See docs/SOUP/tflite-micro.md.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Idempotency guard.
if(DEFINED _RA_TFLITE_MICRO_INCLUDED)
    return()
endif()
set(_RA_TFLITE_MICRO_INCLUDED TRUE)

option(RA_USE_TFLITE_MICRO "Enable the vendored TFLite-micro inference runtime" OFF)

if(NOT RA_USE_TFLITE_MICRO)
    return()
endif()

# TFLite-micro is C++17. The top-level firmware project declares only C + ASM,
# so enable C++ here for any consumer that pulls this file in.
enable_language(CXX)

# Resolve the repo root so this file works whether it is included from the
# top-level CMakeLists.txt or from a standalone per-app build.
get_filename_component(_RA_TFLM_REPO_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_RA_TFLM_DIR    "${_RA_TFLM_REPO_ROOT}/libs/third_party/tflite-micro")
set(_RA_FLATB_DIR   "${_RA_TFLM_REPO_ROOT}/libs/third_party/flatbuffers")
set(_RA_GEMMLOWP_DIR "${_RA_TFLM_REPO_ROOT}/libs/third_party/gemmlowp")
set(_RA_RUY_DIR     "${_RA_TFLM_REPO_ROOT}/libs/third_party/ruy")

if(NOT EXISTS "${_RA_TFLM_DIR}/tensorflow/lite/micro/micro_interpreter.h")
    message(FATAL_ERROR
        "RA_USE_TFLITE_MICRO=ON but the TFLite-micro vendor tree is missing at "
        "${_RA_TFLM_DIR}.")
endif()

if(NOT EXISTS "${_RA_FLATB_DIR}/include/flatbuffers/flatbuffers.h")
    message(FATAL_ERROR
        "RA_USE_TFLITE_MICRO=ON but the FlatBuffers vendor tree is missing at "
        "${_RA_FLATB_DIR}. TFLite-micro's model format needs the FlatBuffer "
        "headers.")
endif()

if(NOT EXISTS "${_RA_GEMMLOWP_DIR}/fixedpoint/fixedpoint.h")
    message(FATAL_ERROR
        "RA_USE_TFLITE_MICRO=ON but the gemmlowp vendor tree is missing at "
        "${_RA_GEMMLOWP_DIR}. The quantized reference kernels need it.")
endif()

if(NOT EXISTS "${_RA_RUY_DIR}/ruy/profiler/instrumentation.h")
    message(FATAL_ERROR
        "RA_USE_TFLITE_MICRO=ON but the ruy vendor tree is missing at "
        "${_RA_RUY_DIR}. The kernel utilities include its profiler stub.")
endif()

# ---------------------------------------------------------------------------
# Source list -- every vendored *.cc compiles into one OBJECT library.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE _RA_TFLM_SOURCES CONFIGURE_DEPENDS
    "${_RA_TFLM_DIR}/tensorflow/*.cc")

add_library(tflite_micro_objs OBJECT ${_RA_TFLM_SOURCES})

# Include roots: the tflite-micro tree root (for "tensorflow/..." includes),
# the FlatBuffer include dir (for "flatbuffers/..."), the gemmlowp root (for
# "fixedpoint/...") and the ruy root (for "ruy/profiler/...").
target_include_directories(tflite_micro_objs PUBLIC
    ${_RA_TFLM_DIR}
    ${_RA_FLATB_DIR}/include
    ${_RA_GEMMLOWP_DIR}
    ${_RA_RUY_DIR})

# TFLite-micro is a static-memory build: TF_LITE_STATIC_MEMORY switches the
# runtime to the no-malloc arena allocator path (NASA Rule 3 friendly).
target_compile_definitions(tflite_micro_objs PUBLIC
    TF_LITE_STATIC_MEMORY)

# C++17, no RTTI / no exceptions (already set by the toolchain CXX flags, pinned
# here so a host-side or non-toolchain consumer still gets them). Function-local
# static guards are disabled: the runtime is single-threaded on the MCU and the
# thread-safe-statics guard drags in __cxa_guard_* from libsupc++.
set_target_properties(tflite_micro_objs PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
target_compile_options(tflite_micro_objs PRIVATE
    -fno-rtti
    -fno-exceptions
    -fno-threadsafe-statics
    -fno-use-cxa-atexit)

# Vendored SOUP: build warning-silent like the other third_party trees, and
# with -fno-strict-aliasing (the kernels type-pun through tensor byte buffers),
# matching the miniz / stb / mbedtls SOUP boundary in cmake/ra_add_app.cmake.
target_compile_options(tflite_micro_objs PRIVATE -w -fno-strict-aliasing)

# Public-facing INTERFACE target. Apps link this; everything else flows through.
add_library(tflite_micro INTERFACE)
target_sources(tflite_micro INTERFACE $<TARGET_OBJECTS:tflite_micro_objs>)
target_include_directories(tflite_micro INTERFACE
    ${_RA_TFLM_DIR}
    ${_RA_FLATB_DIR}/include
    ${_RA_GEMMLOWP_DIR}
    ${_RA_RUY_DIR})
target_compile_definitions(tflite_micro INTERFACE
    TF_LITE_STATIC_MEMORY)

message(STATUS "TFLite-micro enabled: ${_RA_TFLM_DIR}")
message(STATUS "  FlatBuffers:        ${_RA_FLATB_DIR}")
message(STATUS "  gemmlowp:           ${_RA_GEMMLOWP_DIR}")
message(STATUS "  ruy:                ${_RA_RUY_DIR}")
