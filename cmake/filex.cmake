# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/filex.cmake
#
# Top-level integration of the vendored FileX FAT file system. Exposes
# the `RA8_USE_FILEX` option; when ON, this file:
#
#   1. Verifies that ThreadX is also enabled (FileX's
#      multi-thread protection macros call `tx_mutex_*`).
#   2. Compiles `libs/third_party/filex/common/src/*.c` into a single
#      `filex` interface library.
#   3. Pulls in our `port/filex/` shim which bridges FileX onto the
#      `ra8_sdhi` block driver.
#
# Apps that want FileX `target_link_libraries(<app>.elf PRIVATE
# filex filex_port_ra8_sdhi)` -- everything else (include dirs,
# preprocessor defines) flows through the interface.
#
#

# Idempotency guard. The same per-app build can include this file
# more than once if both the top-level CMakeLists.txt and a
# standalone-app build want it; only the first include should run.
if(DEFINED _RA8_FILEX_INCLUDED)
  return()
endif()
set(_RA8_FILEX_INCLUDED TRUE)

option(RA8_USE_FILEX "Enable the vendored FileX FAT file system" OFF)

if(NOT RA8_USE_FILEX)
  return()
endif()

# FileX's protection macros call `tx_mutex_get` / `tx_mutex_put`, so
# the build must also pull in ThreadX. The companion `cmake/threadx.cmake`
# is owned by a sibling / Phase 4.1 agent; if it has not
# landed yet the FileX build cannot succeed and we surface a clear
# error rather than letting the linker complain about missing
# `_tx_*` symbols.
if(NOT RA8_USE_THREADX)
  message(
    FATAL_ERROR
      "RA8_USE_FILEX=ON requires RA8_USE_THREADX=ON. FileX's protection "
      "macros call tx_mutex_get / tx_mutex_put. Enable both options "
      "(or include cmake/threadx.cmake before cmake/filex.cmake)."
  )
endif()

# Resolve the repo root so this file works whether it is included
# from the top-level CMakeLists.txt or from a standalone per-app build.
get_filename_component(_RA8_FILEX_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_RA8_FILEX_VENDOR_DIR "${_RA8_FILEX_REPO_ROOT}/libs/third_party/filex")
set(_RA8_FILEX_COMMON_INC "${_RA8_FILEX_VENDOR_DIR}/common/inc")
set(_RA8_FILEX_COMMON_SRC "${_RA8_FILEX_VENDOR_DIR}/common/src")
set(_RA8_FILEX_PORT_INC "${_RA8_FILEX_VENDOR_DIR}/ports/cortex_m33/gnu/inc")

if(NOT EXISTS "${_RA8_FILEX_COMMON_INC}/fx_api.h")
  message(FATAL_ERROR "RA8_USE_FILEX=ON but FileX vendor tree is missing at "
                      "${_RA8_FILEX_VENDOR_DIR}."
  )
endif()

# Glob FileX common sources. Vendor upstream owns this list; we do
# not curate it. CONFIGURE_DEPENDS forces a re-glob if any source is
# added or removed.
file(GLOB _RA8_FILEX_COMMON_SOURCES CONFIGURE_DEPENDS "${_RA8_FILEX_COMMON_SRC}/*.c")

# Keep the FileX object library private to this scope. Apps consume
# the `filex` INTERFACE target which bundles include dirs + sources.
add_library(filex_objs OBJECT ${_RA8_FILEX_COMMON_SOURCES})

target_include_directories(filex_objs PUBLIC ${_RA8_FILEX_COMMON_INC} ${_RA8_FILEX_PORT_INC})

# fx_port.h pulls in tx_api.h (via #include "tx_api.h"), so FileX's
# own TU compile needs to see ThreadX's include dirs. Inherit them
# from the `threadx` target rather than hard-coding paths.
target_link_libraries(filex_objs PRIVATE threadx)

# Vendor sources predate -Wpedantic / -Werror cleanliness. Drop the
# warning surface to a permissive baseline so the rest of the tree
# can keep -Werror without forking the upstream code.
target_compile_options(filex_objs PRIVATE -w)

add_library(filex INTERFACE)
target_sources(filex INTERFACE $<TARGET_OBJECTS:filex_objs>)
target_include_directories(filex INTERFACE ${_RA8_FILEX_COMMON_INC} ${_RA8_FILEX_PORT_INC})
target_link_libraries(filex INTERFACE threadx)

# Now pull in our ra8_sdhi <-> FileX shim.
add_subdirectory(${_RA8_FILEX_REPO_ROOT}/port/filex ${CMAKE_BINARY_DIR}/port_filex)

message(STATUS "FileX enabled: ${_RA8_FILEX_VENDOR_DIR}")
