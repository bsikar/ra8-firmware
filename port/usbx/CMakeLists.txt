# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# port/usbx/CMakeLists.txt
#
# Defines the `usbx_port_ra8_usb` interface library that exposes the
# bridge between vendored USBX (libs/third_party/usbx) and the
# project's `ra8_usb` register-level driver.
#
# Two source files implement the bridge:
#
#   - ux_dcd_ra8_usb.c   USBX device-controller-driver -> ra8_usb.
#   - ux_hcd_ra8_usb.c   USBX host-controller-driver   -> ra8_usb.
#
# Pulled in by `cmake/usbx.cmake` when `RA8_USE_USBX=ON`. Apps that
# enable USBX link against `usbx_port_ra8_usb`; that target propagates
# the bridge include directory and tracks the source list as a global
# property so per-app builds can splice it into their `add_executable`
# globs (mirrors how `port/nimble/` exposes its sources).
#
#

cmake_minimum_required(VERSION 3.20)

# Header-only interface for source consumers that prefer to compile
# the .c themselves (matching the rest of the per-app glob style).
add_library(usbx_port_ra8_usb INTERFACE)

target_include_directories(usbx_port_ra8_usb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/inc)

# Track the source list as a global property so each app can splice
# it into add_executable() alongside its own boot files.
set_property(
  GLOBAL
  PROPERTY RA8_USBX_PORT_SOURCES
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ux_dcd_ra8_usb.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ux_dcd_ra8_usb_ep.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ux_dcd_ra8_usb_xfer.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ux_dcd_ra8_usb_isr.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ux_dcd_ra8_usb_setup.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ux_dcd_ra8_usb_dvst.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ux_dcd_ra8_usb_dvst_default.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ux_dcd_ra8_usb_irq.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ux_hcd_ra8_usb.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ux_device_class_storage_inquiry.c
)
