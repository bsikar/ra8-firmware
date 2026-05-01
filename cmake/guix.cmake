#
# cmake/guix.cmake
#
# Top-level integration of the vendored Eclipse GUIX UI framework. This
# file exposes the `RA_USE_GUIX` option, builds a `guix` interface
# library out of every C source under `libs/third_party/guix/common/src/`,
# and pulls in our `port/guix/` shim that bridges GUIX's display driver
# entry points onto the project's `ra_glcdc` framebuffer + `ra_gfx`
# software pixel pusher.
#
# Apps opt in with:
#
#     option(RA_USE_GUIX "Link Eclipse GUIX into this app" OFF)
#     if(RA_USE_GUIX)
#         include(${RA_REPO_ROOT}/cmake/guix.cmake)
#         target_link_libraries(<app>.elf PRIVATE guix guix_port_ra_glcdc)
#     endif()
#
# `RA_USE_GUIX=ON` requires `RA_USE_THREADX=ON` because GUIX's caller-
# checking macros and `gx_system_start()` event-loop bind directly to
# `tx_thread_*` / `tx_event_flags_*` symbols.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Idempotency guard: the same configure pass can include this file from
# both the top-level CMakeLists.txt and a per-app standalone build.
if(DEFINED _RA_GUIX_INCLUDED)
    return()
endif()
set(_RA_GUIX_INCLUDED TRUE)

option(RA_USE_GUIX "Enable the vendored Eclipse GUIX UI framework" OFF)

if(NOT RA_USE_GUIX)
    return()
endif()

# GUIX leans on ThreadX for its event loop and caller-checking macros.
# Surface a clear error rather than letting the linker complain about
# `_tx_*` symbols.
if(NOT RA_USE_THREADX)
    message(FATAL_ERROR
        "RA_USE_GUIX=ON requires RA_USE_THREADX=ON. GUIX's gx_system_start "
        "binds to ThreadX's event-flag and thread APIs. Enable both options "
        "(or include cmake/threadx.cmake before cmake/guix.cmake).")
endif()

if(NOT DEFINED RA_REPO_ROOT)
    get_filename_component(RA_REPO_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(_RA_GUIX_VENDOR_DIR "${RA_REPO_ROOT}/libs/third_party/guix")
set(_RA_GUIX_COMMON_INC "${_RA_GUIX_VENDOR_DIR}/common/inc")
set(_RA_GUIX_COMMON_SRC "${_RA_GUIX_VENDOR_DIR}/common/src")
set(_RA_GUIX_PORT_INC   "${_RA_GUIX_VENDOR_DIR}/ports/cortex_m7/gnu/inc")

if(NOT EXISTS "${_RA_GUIX_COMMON_INC}/gx_api.h")
    message(FATAL_ERROR
        "RA_USE_GUIX=ON but GUIX vendor tree is missing at "
        "${_RA_GUIX_VENDOR_DIR}.")
endif()

# Glob all GUIX kernel sources. CONFIGURE_DEPENDS forces a re-glob if a
# source is added or removed.
file(GLOB _RA_GUIX_COMMON_SOURCES CONFIGURE_DEPENDS
    "${_RA_GUIX_COMMON_SRC}/*.c")

# Vendor sources predate -Werror cleanliness. Drop the warning surface
# to a permissive baseline so the rest of the tree can keep -Werror
# without forking the upstream code.
add_library(guix_objs OBJECT ${_RA_GUIX_COMMON_SOURCES})

target_include_directories(guix_objs PUBLIC
    ${_RA_GUIX_COMMON_INC}
    ${_RA_GUIX_PORT_INC})

# `gx_port.h` for Cortex-M7/GNU pulls in `tx_api.h`. Inherit the
# ThreadX include dirs from the threadx target instead of duplicating
# them here.
target_link_libraries(guix_objs PRIVATE threadx)

target_compile_options(guix_objs PRIVATE -w)

add_library(guix INTERFACE)
target_sources(guix INTERFACE $<TARGET_OBJECTS:guix_objs>)
target_include_directories(guix INTERFACE
    ${_RA_GUIX_COMMON_INC}
    ${_RA_GUIX_PORT_INC})
target_link_libraries(guix INTERFACE threadx)

# Now pull in our ra_glcdc <-> GUIX shim.
add_subdirectory(${RA_REPO_ROOT}/port/guix
                 ${CMAKE_BINARY_DIR}/port_guix)

message(STATUS "GUIX enabled: ${_RA_GUIX_VENDOR_DIR}")
