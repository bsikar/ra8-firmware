# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# port/threadx/CMakeLists.txt
#
# This file exists so a per-app build can do
#
#     add_subdirectory(${RA8_REPO_ROOT}/port/threadx threadx_port)
#
# and end up with the same `threadx` static library the top-level
# include in cmake/threadx.cmake produces. Both routes call the same
# helper file so the two paths stay in lock-step.
#
#

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED RA8_REPO_ROOT)
  get_filename_component(RA8_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

include(${RA8_REPO_ROOT}/cmake/threadx.cmake)
