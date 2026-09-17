# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/usbx.cmake
#
# Top-level integration of the vendored USBX USB stack
# (libs/third_party/usbx). Mirrors the shape of
# cmake/threadx.cmake: declares an interface `usbx` library that
# bundles the USBX core sources, exposes the include dirs, and pulls
# in the project's `port/usbx/` shim.
#
# Usage from a per-app CMakeLists.txt:
#
#     option(RA8_USE_USBX "Enable the vendored USBX USB stack" OFF)
#     if(RA8_USE_USBX)
#         include(${RA8_REPO_ROOT}/cmake/usbx.cmake)
#         target_link_libraries(<app>.elf PRIVATE usbx usbx_port_ra8_usb)
#     endif()
#
# What it does:
#
#   - Compiles every C source under
#     libs/third_party/usbx/common/core/src/*.c except for the
#     simulator DCD / HCD (ux_dcd_sim_slave_*.c and
#     ux_hcd_sim_host_*.c) -- our own DCD / HCD bridges live in
#     port/usbx/ and replace those.
#   - Compiles the CDC-ACM device class under
#     usbx_device_classes/src/ux_device_class_cdc_acm_*.c.
#   - Vendor headers + Cortex-M33/GNU port headers go on the public
#     interface so the bridge + apps can `#include "ux_api.h"`.
#   - Forces UX_INCLUDE_USER_DEFINE_FILE so a project-supplied
#     ux_user.h (currently in port/usbx/) shapes USBX's compile-time
#     options.
#
# Requires `RA8_USE_THREADX=ON` because USBX's tx_api.h dependency
# (memory pools, semaphores, threads) cannot be satisfied otherwise.
#
#

if(DEFINED _RA8_USBX_INCLUDED)
  return()
endif()
set(_RA8_USBX_INCLUDED TRUE)

option(RA8_USE_USBX "Enable the vendored USBX USB stack" OFF)

if(NOT RA8_USE_USBX)
  return()
endif()

if(NOT RA8_USE_THREADX)
  message(FATAL_ERROR "RA8_USE_USBX=ON requires RA8_USE_THREADX=ON. USBX's port glue "
                      "calls into ThreadX semaphores / threads. Enable both options."
  )
endif()

if(NOT DEFINED RA8_REPO_ROOT)
  get_filename_component(RA8_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(_RA8_USBX_VENDOR_DIR "${RA8_REPO_ROOT}/libs/third_party/usbx")
set(_RA8_USBX_COMMON_INC "${_RA8_USBX_VENDOR_DIR}/common/core/inc")
set(_RA8_USBX_COMMON_SRC "${_RA8_USBX_VENDOR_DIR}/common/core/src")
set(_RA8_USBX_DEV_CLS_INC "${_RA8_USBX_VENDOR_DIR}/common/usbx_device_classes/inc")
set(_RA8_USBX_DEV_CLS_SRC "${_RA8_USBX_VENDOR_DIR}/common/usbx_device_classes/src")
set(_RA8_USBX_PORT_INC "${_RA8_USBX_VENDOR_DIR}/ports/cortex_m33/gnu/inc")

if(NOT EXISTS "${_RA8_USBX_COMMON_INC}/ux_api.h")
  message(FATAL_ERROR "RA8_USE_USBX=ON but USBX vendor tree is missing at "
                      "${_RA8_USBX_VENDOR_DIR}."
  )
endif()

# Glob the USBX core. Drop the simulator DCD / HCD files: those are
# replaced by our own bridges in port/usbx/.
file(GLOB _RA8_USBX_CORE_SOURCES CONFIGURE_DEPENDS "${_RA8_USBX_COMMON_SRC}/*.c")
list(
  FILTER
  _RA8_USBX_CORE_SOURCES
  EXCLUDE
  REGEX
  ".*/ux_dcd_sim_slave_.*\\.c$"
)
list(
  FILTER
  _RA8_USBX_CORE_SOURCES
  EXCLUDE
  REGEX
  ".*/ux_hcd_sim_host_.*\\.c$"
)

# CDC-ACM device class (used by threadx_usbx_cdc_demo).
file(GLOB _RA8_USBX_CDC_SOURCES CONFIGURE_DEPENDS
     "${_RA8_USBX_DEV_CLS_SRC}/ux_device_class_cdc_acm_*.c"
)

# HID device class (used by usb_hid_device).
file(GLOB _RA8_USBX_HID_SOURCES CONFIGURE_DEPENDS
     "${_RA8_USBX_DEV_CLS_SRC}/ux_device_class_hid_*.c"
)

# Mass-Storage Class (used by usb_msc_device). Drop the PIMA still-image
# files that share the storage_* prefix in some upstream layouts -- they
# are part of the PIMA class, not MSC.
file(GLOB _RA8_USBX_MSC_SOURCES CONFIGURE_DEPENDS
     "${_RA8_USBX_DEV_CLS_SRC}/ux_device_class_storage_*.c"
)
list(
  FILTER
  _RA8_USBX_MSC_SOURCES
  EXCLUDE
  REGEX
  ".*/ux_device_class_pima_storage_.*\\.c$"
)

# The vendored INQUIRY handler reports RESPONSE DATA FORMAT = 0 (SCSI-1)
# and ignores the EVPD bit; macOS's SCSI layer rejects both and abandons
# the device after a BOT reset. Replace that one TU with the SPC-correct
# first-party override in port/usbx/ (same pattern as the DCD/HCD
# bridges above -- the vendor tree itself stays untouched).
list(
  FILTER
  _RA8_USBX_MSC_SOURCES
  EXCLUDE
  REGEX
  ".*/ux_device_class_storage_inquiry\\.c$"
)

# DFU device class (used by usb_selftest_dfu).
file(GLOB _RA8_USBX_DFU_SOURCES CONFIGURE_DEPENDS
     "${_RA8_USBX_DEV_CLS_SRC}/ux_device_class_dfu_*.c"
)

add_library(
  usbx_objs OBJECT
  ${_RA8_USBX_CORE_SOURCES}
  ${_RA8_USBX_CDC_SOURCES}
  ${_RA8_USBX_HID_SOURCES}
  ${_RA8_USBX_MSC_SOURCES}
  ${_RA8_USBX_DFU_SOURCES}
)

target_include_directories(
  usbx_objs SYSTEM PUBLIC ${_RA8_USBX_COMMON_INC} ${_RA8_USBX_DEV_CLS_INC} ${_RA8_USBX_PORT_INC}
)

# tx_api.h is referenced from ux_port.h, so USBX's TUs need ThreadX
# include dirs; inherit them transitively from the threadx target.
target_link_libraries(usbx_objs PRIVATE threadx)

# No warning suppression here, deliberately. This object library DOES carry the
# full project profile (-Wall -Wextra -Werror plus -Wredundant-decls,
# -Wcast-align, -Wcast-qual, -Wstack-usage=2200 and the rest), and it is clean
# under it: compiling all 247 vendored USBX core and device-class TUs with
# -Wno-redundant-decls, -Wno-discarded-qualifiers and -Wno-cast-align removed,
# under arm-none-eabi-gcc 13.3.1 at -O0, emits not one diagnostic. The note
# these flags carried -- "vendor sources predate -Wpedantic / -Werror
# cleanliness" -- stopped being true of this pin, so the flags are deleted
# rather than left standing as a claim about the code that measurement
# contradicts. The per-app usbx suppressions (examples/.../tz_nsc_cgc_usb) are
# NOT redundant with this: that app compiles a different USBX subset with a
# different include posture and its remaining two flags are separately measured.

add_library(usbx INTERFACE)
target_sources(usbx INTERFACE $<TARGET_OBJECTS:usbx_objs>)
target_include_directories(
  usbx SYSTEM INTERFACE ${_RA8_USBX_COMMON_INC} ${_RA8_USBX_DEV_CLS_INC} ${_RA8_USBX_PORT_INC}
)
target_link_libraries(usbx INTERFACE threadx)

# Maximum logical units the device Mass-Storage class supports. The
# cortex_m33 port (ux_port.h) defaults this to 1; raise it so a single
# MSC device can expose several LUNs (the multi-LUN self-loop). The
# value sizes the UX_SLAVE_CLASS_STORAGE[_PARAMETER] LUN arrays, so it
# MUST be identical for the class TUs (usbx_objs) and every app that
# fills the parameter struct -- hence it is set on both the object lib
# and the public interface. Single-LUN apps are unaffected (they use
# LUN 0 of a slightly larger array).
set(RA8_USBX_MAX_PERIPHERAL_LUN
    2
    CACHE STRING "USBX device Mass-Storage class: max logical units per device"
)
target_compile_definitions(usbx_objs PUBLIC UX_MAX_SLAVE_LUN=${RA8_USBX_MAX_PERIPHERAL_LUN})
target_compile_definitions(usbx INTERFACE UX_MAX_SLAVE_LUN=${RA8_USBX_MAX_PERIPHERAL_LUN})

# Device transfer-request buffer / MSC bulk chunk size. The cortex_m33
# port defaults this to 2048; the storage class reads each SCSI WRITE in
# chunks of this size, and the single-buffer device bulk-OUT pipe NAKs
# the host during the brief re-arm gap between consecutive chunks, timing
# out the host. Raising it to 4096 keeps a typical SCSI WRITE inside one
# device transfer (no inter-chunk gap). MUST match usbx_objs and apps.
set(RA8_USBX_REQUEST_DATA_MAX_LENGTH
    4096
    CACHE STRING "USBX device transfer-request buffer / MSC bulk chunk size (bytes)"
)
target_compile_definitions(
  usbx_objs PUBLIC UX_SLAVE_REQUEST_DATA_MAX_LENGTH=${RA8_USBX_REQUEST_DATA_MAX_LENGTH}
)
target_compile_definitions(
  usbx INTERFACE UX_SLAVE_REQUEST_DATA_MAX_LENGTH=${RA8_USBX_REQUEST_DATA_MAX_LENGTH}
)

# Pull in the project's ra8_usb <-> USBX bridge.
add_subdirectory(${RA8_REPO_ROOT}/port/usbx ${CMAKE_BINARY_DIR}/port_usbx)

message(STATUS "USBX enabled: ${_RA8_USBX_VENDOR_DIR}")
