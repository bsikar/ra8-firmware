# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/levelx.cmake
#
# Top-level integration of the vendored LevelX wear-levelling library.
# Exposes the `RA8_USE_LEVELX` option; when ON, this file:
#
#   1. Verifies that ThreadX is also enabled. `lx_api.h` includes
#      `tx_api.h` whenever `LX_STANDALONE_ENABLE` is absent
#      (`common/inc/lx_api.h:69-72`), so every LevelX TU in this mode
#      needs the ThreadX headers, mutexes or not.
#   2. Compiles `libs/third_party/levelx/common/src/lx_nor_*.c` into a
#      single `levelx` interface library. NAND sources and the
#      simulator drivers are excluded -- this firmware uses LevelX's
#      NOR API exclusively, against the on-board MX25LM512 chip.
#   3. Pulls in our `port/levelx/` shim which bridges LevelX onto
#      `ra8_xspi`.
#
# Apps that want LevelX `target_link_libraries(<app>.elf PRIVATE
# levelx levelx_port_ra8_xspi)`. Everything else (include dirs,
# preprocessor defines) flows through the interface.
#
#

# Idempotency guard. The same per-app build can include this file
# more than once if both the top-level CMakeLists.txt and a
# standalone-app build want it; only the first include should run.
if(DEFINED _RA8_LEVELX_INCLUDED)
  return()
endif()
set(_RA8_LEVELX_INCLUDED TRUE)

option(RA8_USE_LEVELX "Enable the vendored LevelX NOR wear-levelling library" OFF)

if(NOT RA8_USE_LEVELX)
  return()
endif()

# This mode does not define `LX_STANDALONE_ENABLE`, so `lx_api.h`
# includes `tx_api.h` (`common/inc/lx_api.h:69-72`) and every LevelX TU
# compiled here needs ThreadX in the graph. That include is the whole
# dependency: LevelX's `tx_mutex_get` / `tx_mutex_put` calls sit behind
# `LX_THREAD_SAFE_ENABLE`, which nothing in this repository defines, so
# no mutex code is compiled in either mode. Use
# cmake/levelx_standalone.cmake for a LevelX build with no ThreadX.
# Surface a clear error if ThreadX is not enabled.
if(NOT RA8_USE_THREADX)
  message(
    FATAL_ERROR
      "RA8_USE_LEVELX=ON requires RA8_USE_THREADX=ON: without "
      "LX_STANDALONE_ENABLE, lx_api.h includes tx_api.h, so the LevelX "
      "sources will not compile. Enable both options (or include "
      "cmake/threadx.cmake before cmake/levelx.cmake), or use "
      "RA8_USE_LEVELX_STANDALONE=ON for a build with no ThreadX."
  )
endif()

# Resolve the repo root so this file works whether it is included
# from the top-level CMakeLists.txt or from a standalone per-app build.
get_filename_component(_RA8_LEVELX_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_RA8_LEVELX_VENDOR_DIR "${_RA8_LEVELX_REPO_ROOT}/libs/third_party/levelx")
set(_RA8_LEVELX_COMMON_INC "${_RA8_LEVELX_VENDOR_DIR}/common/inc")
set(_RA8_LEVELX_COMMON_SRC "${_RA8_LEVELX_VENDOR_DIR}/common/src")

if(NOT EXISTS "${_RA8_LEVELX_COMMON_INC}/lx_api.h")
  message(FATAL_ERROR "RA8_USE_LEVELX=ON but LevelX vendor tree is missing at "
                      "${_RA8_LEVELX_VENDOR_DIR}."
  )
endif()

# Glob LevelX NOR-only sources. The NAND tree and the simulator
# drivers (`fx_*` + `lx_*_simulator*`) are excluded -- this firmware
# uses LevelX exclusively to wear-level the MX25LM512 NOR chip via our
# `port/levelx/src/lx_nor_driver_ra8_xspi.c` shim, so dragging the NAND
# code in only inflates the build and the warning surface.
file(GLOB _RA8_LEVELX_NOR_SOURCES CONFIGURE_DEPENDS "${_RA8_LEVELX_COMMON_SRC}/lx_nor_*.c")

# Drop the upstream simulator (it ships another `_lx_nor_flash_simulator_*`
# definition that would clash with downstream firmware images that pick
# their own NOR driver). It lives under the same `lx_nor_*` glob.
list(
  FILTER
  _RA8_LEVELX_NOR_SOURCES
  EXCLUDE
  REGEX
  ".*/lx_nor_flash_simulator\\.c$"
)

# Keep the LevelX object library private to this scope. Apps consume
# the `levelx` INTERFACE target which bundles include dirs + sources.
add_library(levelx_objs OBJECT ${_RA8_LEVELX_NOR_SOURCES})

target_include_directories(levelx_objs PUBLIC ${_RA8_LEVELX_COMMON_INC})

# `lx_api.h` pulls in `tx_api.h` (via `#include "tx_api.h"`, guarded
# only by `#ifndef LX_STANDALONE_ENABLE`), so LevelX's own TU compile
# needs to see ThreadX's include dirs. Inherit them from the `threadx`
# target rather than hard-coding paths.
target_link_libraries(levelx_objs PRIVATE threadx)

# Vendor sources predate `-Wpedantic` / `-Werror` cleanliness. Drop
# the warning surface to a permissive baseline so the rest of the
# tree can keep `-Werror` without forking the upstream code.

add_library(levelx INTERFACE)
target_sources(levelx INTERFACE $<TARGET_OBJECTS:levelx_objs>)
target_include_directories(levelx INTERFACE ${_RA8_LEVELX_COMMON_INC})
target_link_libraries(levelx INTERFACE threadx)

# Now pull in our ra8_xspi <-> LevelX shim.
add_subdirectory(${_RA8_LEVELX_REPO_ROOT}/port/levelx ${CMAKE_BINARY_DIR}/port_levelx)

message(STATUS "LevelX enabled: ${_RA8_LEVELX_VENDOR_DIR}")
