#
# port/nimble/CMakeLists.txt
#
# Defines the `nimble_port_threadx` interface library that exposes
# our two adapter source files to the per-app build:
#
#   - `ble_hci_ra8_ble.c`    -- bridges NimBLE's HCI transport API
#                              onto our `ra8_ble` HCI ring (host ->
#                              controller HCI commands + ACL data
#                              outbound, controller -> host events
#                              + ACL data inbound).
#   - `nimble_npl_threadx.c` -- ThreadX-backed implementation of the
#                              NimBLE Native Porting Layer (mutex /
#                              semaphore / event-queue / callout
#                              primitives that NimBLE expects from
#                              the host RTOS).
#
# Pulled in by `cmake/nimble.cmake` when `RA8_USE_NIMBLE=ON`. The
# example app links against `nimble_port_threadx` -- it brings its
# own globbed `ra8_hal` sources, so this shim only needs to surface
# the include dir + the source list as a GLOBAL property the per-app
# CMakeLists splices into its own add_executable() call.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Header-only interface for source consumers that prefer to compile
# the .c themselves (mirroring the rest of the per-app glob style).
add_library(nimble_port_threadx INTERFACE)

target_include_directories(nimble_port_threadx INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(nimble_port_threadx INTERFACE
    nimble
    threadx
)

# Track the source list as a property so apps can splice it into
# their own add_executable() globs. Same pattern used by
# port/filex/CMakeLists.txt.
set_property(GLOBAL PROPERTY RA8_NIMBLE_PORT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ble_hci_ra8_ble.c
    ${CMAKE_CURRENT_SOURCE_DIR}/nimble_npl_threadx.c
)
