#
# cmake/levelx_standalone.cmake
#
# Standalone (no-ThreadX) build of the vendored LevelX NOR wear-levelling
# library, the companion to cmake/levelx.cmake (which couples LevelX to ThreadX
# + the ra8_xspi bridge). Exposes the `RA8_USE_LEVELX_STANDALONE` option; when
# ON, this file:
#
#   1. Compiles `libs/third_party/levelx/common/src/lx_nor_*.c` into a single
#      `levelx_standalone` interface library, built with `LX_STANDALONE_ENABLE`
#      so LevelX's protection macros compile to no-ops (no `tx_mutex_*`, hence
#      NO ThreadX dependency). NAND sources and the upstream simulator driver
#      are excluded -- this firmware uses LevelX's NOR API only.
#   2. Propagates `LX_STANDALONE_ENABLE` and the LevelX common/inc include path
#      to every consumer, so an app's own TUs (and any first-party library it
#      pulls in, e.g. ra8_cache_store) see the standalone `lx_api.h` shape.
#
# Unlike cmake/levelx.cmake this file pulls in NO board port: the caller supplies
# its own LevelX NOR driver-initialise callback (an `ra8_cache_store_nor_init_fn`
# / LevelX driver-init function). Production binds the real Octo-SPI driver; the
# ra8_cache_store_demo example binds a RAM-backed NOR driver so the whole path
# runs in SRAM (identical on hardware and in ra8_emulator -- no MMIO to model).
#
# Apps opt in with `ra8_add_app(... USES levelx_standalone ...)`, whose per-app
# Makefile forces `-DRA8_USE_LEVELX_STANDALONE=ON`. The two LevelX build modes
# (ThreadX-coupled `levelx` vs standalone `levelx_standalone`) are mutually
# exclusive within one build; an app selects exactly one.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Idempotency guard: the same per-app build can include this file more than once
# (top-level CMakeLists plus a standalone per-app build); only the first runs.
if(DEFINED _RA8_LEVELX_STANDALONE_INCLUDED)
  return()
endif()
set(_RA8_LEVELX_STANDALONE_INCLUDED TRUE)

option(RA8_USE_LEVELX_STANDALONE
       "Enable the vendored LevelX NOR library in standalone (no-ThreadX) mode" OFF
)

if(NOT RA8_USE_LEVELX_STANDALONE)
  return()
endif()

# LevelX's ThreadX-coupled build and its standalone build cannot coexist in one
# image: both compile the same lx_nor_*.c TUs, so linking both would multiply
# define every LevelX symbol. Surface that clearly instead of a confusing link
# error.
if(RA8_USE_LEVELX)
  message(
    FATAL_ERROR
      "RA8_USE_LEVELX_STANDALONE and RA8_USE_LEVELX are mutually exclusive: "
      "both compile libs/third_party/levelx/common/src/lx_nor_*.c. Pick one "
      "LevelX build mode per app."
  )
endif()

# Resolve the repo root so this file works whether included from the top-level
# CMakeLists.txt or from a standalone per-app build.
get_filename_component(_RA8_LXS_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_RA8_LXS_VENDOR_DIR "${_RA8_LXS_REPO_ROOT}/libs/third_party/levelx")
set(_RA8_LXS_COMMON_INC "${_RA8_LXS_VENDOR_DIR}/common/inc")
set(_RA8_LXS_COMMON_SRC "${_RA8_LXS_VENDOR_DIR}/common/src")

if(NOT EXISTS "${_RA8_LXS_COMMON_INC}/lx_api.h")
  message(FATAL_ERROR "RA8_USE_LEVELX_STANDALONE=ON but LevelX vendor tree is missing at "
                      "${_RA8_LXS_VENDOR_DIR}."
  )
endif()

# Glob the LevelX NOR-only sources (the NAND tree is unused), then drop the
# upstream simulator: this build supplies its own NOR driver, and the simulator
# ships another `_lx_nor_flash_simulator_*` definition that would clash.
file(GLOB _RA8_LXS_NOR_SOURCES CONFIGURE_DEPENDS "${_RA8_LXS_COMMON_SRC}/lx_nor_*.c")
list(
  FILTER
  _RA8_LXS_NOR_SOURCES
  EXCLUDE
  REGEX
  ".*/lx_nor_flash_simulator\\.c$"
)

# Compile the LevelX NOR sources standalone. The object library is private to
# this scope; consumers link the `levelx_standalone` INTERFACE target below.
add_library(levelx_standalone_objs OBJECT ${_RA8_LXS_NOR_SOURCES})
target_include_directories(levelx_standalone_objs PUBLIC ${_RA8_LXS_COMMON_INC})
target_compile_definitions(levelx_standalone_objs PRIVATE LX_STANDALONE_ENABLE)

# Vendor sources predate the project's -Wpedantic / -Werror cleanliness. Drop
# the warning surface to a permissive baseline (matches cmake/levelx.cmake) so
# the rest of the tree keeps -Werror without forking upstream code.
target_compile_options(levelx_standalone_objs PRIVATE -w)

add_library(levelx_standalone INTERFACE)
target_sources(levelx_standalone INTERFACE $<TARGET_OBJECTS:levelx_standalone_objs>)
target_include_directories(levelx_standalone INTERFACE ${_RA8_LXS_COMMON_INC})
# Consumers (main.c, the RAM NOR driver, ra8_cache_store's TUs) must see the
# same standalone lx_api.h shape the object library was built with.
target_compile_definitions(levelx_standalone INTERFACE LX_STANDALONE_ENABLE)

message(STATUS "LevelX (standalone, no-ThreadX) enabled: ${_RA8_LXS_VENDOR_DIR}")
