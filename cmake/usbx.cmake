#
# cmake/usbx.cmake
#
# Top-level integration of the vendored USBX USB stack
# (libs/third_party/usbx). Mirrors the shape of cmake/filex.cmake +
# cmake/threadx.cmake: declares an interface `usbx` library that
# bundles the USBX core sources, exposes the include dirs, and pulls
# in the project's `port/usbx/` shim.
#
# Usage from a per-app CMakeLists.txt:
#
#     option(RA_USE_USBX "Enable the vendored USBX USB stack" OFF)
#     if(RA_USE_USBX)
#         include(${RA_REPO_ROOT}/cmake/usbx.cmake)
#         target_link_libraries(<app>.elf PRIVATE usbx usbx_port_ra_usb)
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
# Requires `RA_USE_THREADX=ON` because USBX's tx_api.h dependency
# (memory pools, semaphores, threads) cannot be satisfied otherwise.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

if(DEFINED _RA_USBX_INCLUDED)
    return()
endif()
set(_RA_USBX_INCLUDED TRUE)

option(RA_USE_USBX "Enable the vendored USBX USB stack" OFF)

if(NOT RA_USE_USBX)
    return()
endif()

if(NOT RA_USE_THREADX)
    message(FATAL_ERROR
        "RA_USE_USBX=ON requires RA_USE_THREADX=ON. USBX's port glue "
        "calls into ThreadX semaphores / threads. Enable both options.")
endif()

if(NOT DEFINED RA_REPO_ROOT)
    get_filename_component(RA_REPO_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(_RA_USBX_VENDOR_DIR  "${RA_REPO_ROOT}/libs/third_party/usbx")
set(_RA_USBX_COMMON_INC  "${_RA_USBX_VENDOR_DIR}/common/core/inc")
set(_RA_USBX_COMMON_SRC  "${_RA_USBX_VENDOR_DIR}/common/core/src")
set(_RA_USBX_DEV_CLS_INC "${_RA_USBX_VENDOR_DIR}/common/usbx_device_classes/inc")
set(_RA_USBX_DEV_CLS_SRC "${_RA_USBX_VENDOR_DIR}/common/usbx_device_classes/src")
set(_RA_USBX_PORT_INC    "${_RA_USBX_VENDOR_DIR}/ports/cortex_m33/gnu/inc")

if(NOT EXISTS "${_RA_USBX_COMMON_INC}/ux_api.h")
    message(FATAL_ERROR
        "RA_USE_USBX=ON but USBX vendor tree is missing at "
        "${_RA_USBX_VENDOR_DIR}.")
endif()

# Glob the USBX core. Drop the simulator DCD / HCD files: those are
# replaced by our own bridges in port/usbx/.
file(GLOB _RA_USBX_CORE_SOURCES CONFIGURE_DEPENDS
    "${_RA_USBX_COMMON_SRC}/*.c")
list(FILTER _RA_USBX_CORE_SOURCES
    EXCLUDE REGEX ".*/ux_dcd_sim_slave_.*\\.c$")
list(FILTER _RA_USBX_CORE_SOURCES
    EXCLUDE REGEX ".*/ux_hcd_sim_host_.*\\.c$")

# CDC-ACM device class (used by threadx_usbx_cdc_demo).
file(GLOB _RA_USBX_CDC_SOURCES CONFIGURE_DEPENDS
    "${_RA_USBX_DEV_CLS_SRC}/ux_device_class_cdc_acm_*.c")

# HID device class (used by usb_hid_device).
file(GLOB _RA_USBX_HID_SOURCES CONFIGURE_DEPENDS
    "${_RA_USBX_DEV_CLS_SRC}/ux_device_class_hid_*.c")

# Mass-Storage Class (used by usb_msc_device). Drop the PIMA still-image
# files that share the storage_* prefix in some upstream layouts -- they
# are part of the PIMA class, not MSC.
file(GLOB _RA_USBX_MSC_SOURCES CONFIGURE_DEPENDS
    "${_RA_USBX_DEV_CLS_SRC}/ux_device_class_storage_*.c")
list(FILTER _RA_USBX_MSC_SOURCES
    EXCLUDE REGEX ".*/ux_device_class_pima_storage_.*\\.c$")

# The vendored INQUIRY handler reports RESPONSE DATA FORMAT = 0 (SCSI-1)
# and ignores the EVPD bit; macOS's SCSI layer rejects both and abandons
# the device after a BOT reset. Replace that one TU with the SPC-correct
# first-party override in port/usbx/ (same pattern as the DCD/HCD
# bridges above -- the vendor tree itself stays untouched).
list(FILTER _RA_USBX_MSC_SOURCES
    EXCLUDE REGEX ".*/ux_device_class_storage_inquiry\\.c$")

add_library(usbx_objs OBJECT
    ${_RA_USBX_CORE_SOURCES}
    ${_RA_USBX_CDC_SOURCES}
    ${_RA_USBX_HID_SOURCES}
    ${_RA_USBX_MSC_SOURCES})

target_include_directories(usbx_objs PUBLIC
    ${_RA_USBX_COMMON_INC}
    ${_RA_USBX_DEV_CLS_INC}
    ${_RA_USBX_PORT_INC})

# tx_api.h is referenced from ux_port.h, so USBX's TUs need ThreadX
# include dirs; inherit them transitively from the threadx target.
target_link_libraries(usbx_objs PRIVATE threadx)

# Vendor sources predate -Wpedantic / -Werror cleanliness. Keep them
# permissive so the rest of the firmware can keep -Werror on without
# forking the upstream code.
target_compile_options(usbx_objs PRIVATE
    -w
    -Wno-error)

add_library(usbx INTERFACE)
target_sources(usbx INTERFACE $<TARGET_OBJECTS:usbx_objs>)
target_include_directories(usbx INTERFACE
    ${_RA_USBX_COMMON_INC}
    ${_RA_USBX_DEV_CLS_INC}
    ${_RA_USBX_PORT_INC})
target_link_libraries(usbx INTERFACE threadx)

# Pull in the project's ra_usb <-> USBX bridge.
add_subdirectory(${RA_REPO_ROOT}/port/usbx
                 ${CMAKE_BINARY_DIR}/port_usbx)

message(STATUS "USBX enabled: ${_RA_USBX_VENDOR_DIR}")
