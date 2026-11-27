# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/nimble.cmake
#
# Top-level integration of the Apache NimBLE host stack on RA8. NimBLE is
# not vendored: it is pinned in build.zig.zon (upstream nimble_1_10_0_tag
# tarball, url + hash) and fetched into the Zig package cache by
# cmake/zig_package.cmake. Exposes the `RA8_USE_NIMBLE` option; when ON, this file:
#
#   1. Verifies that ThreadX is also enabled (the NimBLE Native
#      Porting Layer maps onto ThreadX TX_MUTEX / TX_SEMAPHORE /
#      TX_QUEUE / TX_TIMER primitives).
#   2. Pulls in our `port/nimble/` adapter which:
#        - Implements the NPL primitives on top of ThreadX.
#        - Bridges NimBLE's HCI transport entry points
#          (ble_transport_to_ll_cmd / ble_transport_to_ll_acl) onto
#          our `ra8_ble` HCI ring (ra8_ble_hci_send_command /
#          ra8_ble_hci_send_acl_data) and pumps inbound HCI events /
#          ACL frames back into NimBLE via
#          ble_transport_to_hs_evt / ble_transport_to_hs_acl.
#   3. Compiles the upstream NimBLE 1.10.0 host stack, GAP/GATT/BAS/DIS
#      services, RAM config store, HCI transport pools, and OS porting
#      primitives (`porting/nimble/src` minus controller hal_timer/cputime,
#      plus `nimble/transport/src` minus `rx_task.c`, and
#      `nimble/host/src` minus `ble_gatts_lcl.c` which requires console.h)
#      into `nimble_objs`, surfaced through the `nimble` INTERFACE library
#      so the per-app build links `nimble nimble_port_threadx`.
#
# Apps that want NimBLE link `nimble nimble_port_threadx` -- the
# include dirs, compiled host objects, and ThreadX dependency flow
# through the interface.
#

# Idempotency guard. The same per-app build can include this file
# more than once if both the top-level CMakeLists.txt and a
# standalone-app build want it; only the first include should run.
if(DEFINED _RA8_NIMBLE_INCLUDED)
  return()
endif()
set(_RA8_NIMBLE_INCLUDED TRUE)

option(RA8_USE_NIMBLE "Enable the Apache NimBLE host stack (pinned build.zig.zon package)" OFF)

if(NOT RA8_USE_NIMBLE)
  return()
endif()

# NimBLE's Native Porting Layer (NPL) is implemented on top of
# ThreadX TX_MUTEX / TX_SEMAPHORE / TX_QUEUE / TX_TIMER primitives,
# so the build must also pull in ThreadX. The companion
# `cmake/threadx.cmake` is owned by ; if it has not landed
# yet the NimBLE build cannot succeed and we surface a clear error
# rather than letting the linker complain about missing `_tx_*`
# symbols.
if(NOT RA8_USE_THREADX)
  message(
    FATAL_ERROR
      "RA8_USE_NIMBLE=ON requires RA8_USE_THREADX=ON. The NimBLE NPL "
      "is implemented on top of TX_MUTEX / TX_SEMAPHORE / TX_QUEUE / "
      "TX_TIMER. Enable both options (or include cmake/threadx.cmake "
      "before cmake/nimble.cmake)."
  )
endif()

# Resolve the repo root so this file works whether it is included
# from the top-level CMakeLists.txt or from a standalone per-app build.
get_filename_component(_RA8_NIMBLE_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

include(${_RA8_NIMBLE_REPO_ROOT}/cmake/zig_package.cmake)
ra8_zig_package_dir(nimble _RA8_NIMBLE_VENDOR_DIR)

if(NOT EXISTS "${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/include/host/ble_hs.h")
  message(FATAL_ERROR "RA8_USE_NIMBLE=ON but the pinned Apache NimBLE package is missing at "
                      "${_RA8_NIMBLE_VENDOR_DIR}."
  )
endif()

set(_RA8_NIMBLE_INCLUDE_DIRS
    # Put port/nimble/inc ahead of upstream porting/nimble/include so
    # port/nimble/inc/syscfg/syscfg.h, nimble/nimble_npl_os.h, and
    # nimble/nimble_npl_os_log.h pre-empt upstream fallbacks cleanly.
    ${_RA8_NIMBLE_REPO_ROOT}/port/nimble/inc
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/include
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/include
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/services/bas/include
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/services/dis/include
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/services/gap/include
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/services/gatt/include
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/store/config/include
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/util/include
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/transport/include
    ${_RA8_NIMBLE_VENDOR_DIR}/porting/nimble/include
)

# Host, services, config store, transport pools, and OS primitives from pinned
# Apache NimBLE 1.10.0. Exclude ble_gatts_lcl.c (requires console/console.h)
# and controller-only timer/cputime/rx_task TUs.
file(GLOB _RA8_NIMBLE_HOST_SOURCES CONFIGURE_DEPENDS
     "${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/src/*.c"
)
list(
  FILTER
  _RA8_NIMBLE_HOST_SOURCES
  EXCLUDE
  REGEX
  ".*/ble_gatts_lcl\\.c$"
)

set(_RA8_NIMBLE_SOURCES
    ${_RA8_NIMBLE_HOST_SOURCES}
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/util/src/addr.c
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/services/bas/src/ble_svc_bas.c
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/services/dis/src/ble_svc_dis.c
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/services/gap/src/ble_svc_gap.c
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/services/gatt/src/ble_svc_gatt.c
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/store/config/src/ble_store_config.c
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/store/config/src/ble_store_config_conf.c
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/transport/src/monitor.c
    ${_RA8_NIMBLE_VENDOR_DIR}/nimble/transport/src/transport.c
    ${_RA8_NIMBLE_VENDOR_DIR}/porting/nimble/src/endian.c
    ${_RA8_NIMBLE_VENDOR_DIR}/porting/nimble/src/mem.c
    ${_RA8_NIMBLE_VENDOR_DIR}/porting/nimble/src/nimble_port.c
    ${_RA8_NIMBLE_VENDOR_DIR}/porting/nimble/src/os_mbuf.c
    ${_RA8_NIMBLE_VENDOR_DIR}/porting/nimble/src/os_mempool.c
    ${_RA8_NIMBLE_VENDOR_DIR}/porting/nimble/src/os_msys.c
)

add_library(nimble_objs OBJECT ${_RA8_NIMBLE_SOURCES})

target_compile_definitions(nimble_objs PUBLIC RA8_TARGET_BUILD)

target_include_directories(nimble_objs SYSTEM PUBLIC ${_RA8_NIMBLE_INCLUDE_DIRS})
target_include_directories(
  nimble_objs SYSTEM PRIVATE ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/src
                             ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/store/config/src
)

target_link_libraries(nimble_objs PUBLIC threadx)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_NAME STREQUAL "Generic")
  target_compile_options(
    nimble_objs PRIVATE -fshort-enums -ffreestanding -DNDEBUG
                        -include${_RA8_NIMBLE_REPO_ROOT}/port/nimble/inc/nimble_port_alloc.h
  )
endif()

add_library(nimble INTERFACE)

target_compile_definitions(nimble INTERFACE RA8_TARGET_BUILD)

target_include_directories(nimble SYSTEM INTERFACE ${_RA8_NIMBLE_INCLUDE_DIRS})

target_sources(nimble INTERFACE $<TARGET_OBJECTS:nimble_objs>)

# NimBLE's NPL header pulls ThreadX primitives in via our
# port/nimble/inc/nimble_npl_threadx.h shim. Inherit the ThreadX include
# dirs from the `threadx` target rather than hard-coding them.
target_link_libraries(nimble INTERFACE threadx)

# Pull in our ra8_ble <-> NimBLE transport adapter and the ThreadX-
# backed NPL shim. Defines the `nimble_port_threadx` library that
# carries `ble_hci_ra8_ble.c` and `nimble_npl_threadx.c`.
add_subdirectory(${_RA8_NIMBLE_REPO_ROOT}/port/nimble ${CMAKE_BINARY_DIR}/port_nimble)

message(STATUS "NimBLE enabled: ${_RA8_NIMBLE_VENDOR_DIR}")
