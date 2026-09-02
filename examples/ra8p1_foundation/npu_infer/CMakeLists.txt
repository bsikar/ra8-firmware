# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `npu_infer` -- the RA8P1 Ethos-U55 inference-runtime app.
#
# Build it STANDALONE with the RA8P1 toolchain (it is excluded from the top-level
# RA8D2 unified build, like every ra8p1_foundation app):
#
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8p1.cmake
#   cmake --build build
#
# or simply `just` in this directory (its justfile points at toolchain-ra8p1).
#
# `USES tflite_micro` pulls in cmake/tflite_micro.cmake, which builds the vendored
# TensorFlow Lite for Microcontrollers runtime (plus the first-party Ethos-U
# kernel ra8_ethosu_kernel.cc that replaces the vendored Register_ETHOSU stub) as
# an OBJECT library and links it here. That makes this app the proof that
# RA8_USE_TFLITE_MICRO builds + links end-to-end for the RA8P1, and that the real
# tflite::Register_ETHOSU() is wired to the NPU (asserted at runtime via
# ra8_ethosu_kernel_available()).
#
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(npu_infer LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME npu_infer
  STACK_BYTES 3072
  USES tflite_micro
  DESCRIPTION "RA8P1 Ethos-U55 inference runtime: quantize + IRQ + TFLite-micro (R7KA8P1KFLCAC)"
)
