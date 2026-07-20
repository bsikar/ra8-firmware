#
# Per-app CMake stub for `tls_client`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level
# listfile, which CMake requires to contain a literal, direct project()
# call. The aggregate repo-root build instead pulls this app in via
# add_subdirectory() (project() is already called at the root), so guard on
# PARENT_DIRECTORY to avoid a redundant nested sub-project.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(tls_client LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# USES pulls in the vendored middleware (ThreadX + NetX Duo + Mbed TLS);
# LIBS globs the first-party ra8_tls facade sources into the app so the demo
# talks to the TLS stack through the ra8_tls_* API instead of raw mbedtls_*.
ra8_add_app(
  NAME tls_client
  STACK_BYTES 4096
  DESCRIPTION "TLS client over ra8_tls + ra8_net_pal (NetX Duo transport) for RA8D2"
  USES threadx netxduo mbedtls
  LIBS ra8_tls
)
