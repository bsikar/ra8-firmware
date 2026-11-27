# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# port/nimble/CMakeLists.txt
#
# Defines the `nimble_port_threadx` interface library that exposes
# our adapter source files to the per-app build:
#
#   - `ble_hci_ra8_ble.c`    -- bridges NimBLE's HCI transport API
#                              onto our `ra8_ble` HCI ring (host ->
#                              controller HCI commands + ACL data
#                              outbound, controller -> host events
#                              + ACL data inbound).
#   - `ble_hci_c6link.c`     -- optional binding routing `ra8_ble` onto
#                              `ra8_c6link` ESP32-C6 SPI (`ESP_HCI_IF`).
#   - `nimble_npl_threadx.c` -- ThreadX-backed implementation of the
#                              NimBLE Native Porting Layer (mutex /
#                              semaphore / event-queue / callout /
#                              critical-section / timebase).
#
# Pulled in by `cmake/nimble.cmake` when `RA8_USE_NIMBLE=ON`, alongside
# the compiled upstream `nimble_objs` host stack objects.

cmake_minimum_required(VERSION 3.20)

# Header-only interface for source consumers that prefer to compile
# the .c themselves (mirroring the rest of the per-app glob style).
add_library(nimble_port_threadx INTERFACE)

target_include_directories(nimble_port_threadx INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/inc)

target_link_libraries(nimble_port_threadx INTERFACE nimble threadx)

# Track the source list as a property so apps can splice it into
# their own add_executable() globs. Same pattern used by
# port/usbx/CMakeLists.txt.
set_property(
  GLOBAL
  PROPERTY RA8_NIMBLE_PORT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/ble_hci_ra8_ble.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/nimble_npl_threadx.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/nimble_port_alloc.c
)

set_property(
  GLOBAL PROPERTY RA8_NIMBLE_C6LINK_PORT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/ble_hci_c6link.c
)
