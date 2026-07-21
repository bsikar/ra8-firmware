#
# cmake/shared_libs/CMakeLists.txt -- build the universal first-party static
# archive ONCE for the CI cross-build fast path.
#
# The "Cross-build all apps" gate used to recompile ~180 identical first-party
# library sources (ra8_core / ra8_hal / ra8_net_pal / ra8_usb_pal / board /
# secure_app) once per app across ~200 apps. This project compiles them a
# single time into libra8_shared_<board>.a; the per-app recipe
# (cmake/ra8_add_app.cmake) then links that archive with --whole-archive
# instead of recompiling the sources (see the RA8_SHARED_LIB_ARCHIVE env-var
# path there). --whole-archive + the toolchain's --gc-sections yields the same
# final ELF sections as compiling the sources in directly.
#
# Configure + build (the toolchain, build type, and strict-toolchain pin match
# the per-app builds so the archived objects are identical):
#   cmake -S cmake/shared_libs -B build/shared_libs \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8d2.cmake \
#       -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build/shared_libs --parallel
#
# scripts/build/all_examples.sh drives this automatically.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# Repo root = two levels up from this listfile (cmake/shared_libs/).
get_filename_component(RA8_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

file(READ "${RA8_REPO_ROOT}/VERSION" _ra8_ver)
string(STRIP "${_ra8_ver}" _ra8_ver)

project(
  ra8_shared_libs
  LANGUAGES C ASM
  VERSION ${_ra8_ver}
  DESCRIPTION "Prebuilt universal first-party library archive (cross-build fast path)"
)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE
      RelWithDebInfo
      CACHE STRING "Build type" FORCE
  )
endif()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Match the per-app build-type flag sets (cmake/ra8_add_app.cmake / top-level
# CMakeLists.txt) so the archived objects are byte-for-behaviour identical.
set(CMAKE_C_FLAGS_DEBUG "-O0 -g3 -DDEBUG")
set(CMAKE_ASM_FLAGS_DEBUG "-g3")
set(CMAKE_C_FLAGS_RELEASE "-Os -g1 -DNDEBUG")
set(CMAKE_ASM_FLAGS_RELEASE "-g1")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-Og -g3")
set(CMAKE_ASM_FLAGS_RELWITHDEBINFO "-g3")

# The board the cross-build archive targets. The CI cross-build set is entirely
# ek_ra8d2 (the ra8p1 tier is a different chip, built standalone), so one
# ek_ra8d2 archive serves every eligible app.
set(RA8_SHARED_LIB_BOARD
    "ek_ra8d2"
    CACHE STRING "Board layer for the shared archive"
)

include(${RA8_REPO_ROOT}/cmake/ra8_shared_libs.cmake)

ra8_add_shared_lib_archive(
  NAME ra8_shared_${RA8_SHARED_LIB_BOARD}
  REPO_ROOT ${RA8_REPO_ROOT}
  BOARD ${RA8_SHARED_LIB_BOARD}
)

message(
  STATUS
    "ra8_shared_libs: archive ra8_shared_${RA8_SHARED_LIB_BOARD} (board ${RA8_SHARED_LIB_BOARD})"
)
