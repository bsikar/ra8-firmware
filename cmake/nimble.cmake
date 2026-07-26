#
# cmake/nimble.cmake
#
# Top-level integration of the vendored Apache NimBLE host stack on
# RA8. Exposes the `RA8_USE_NIMBLE` option; when ON, this file:
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
#   3. Compiles a curated subset of the upstream NimBLE host sources
#      into a single `nimble` interface library so the per-app build
#      can `target_link_libraries(<app>.elf PRIVATE nimble nimble_port_threadx)`.
#
# Apps that want NimBLE link `nimble nimble_port_threadx` -- the
# include dirs and ThreadX dependency flow through the interface.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Idempotency guard. The same per-app build can include this file
# more than once if both the top-level CMakeLists.txt and a
# standalone-app build want it; only the first include should run.
if(DEFINED _RA8_NIMBLE_INCLUDED)
  return()
endif()
set(_RA8_NIMBLE_INCLUDED TRUE)

option(RA8_USE_NIMBLE "Enable the vendored Apache NimBLE host stack" OFF)

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

set(_RA8_NIMBLE_VENDOR_DIR "${_RA8_NIMBLE_REPO_ROOT}/libs/third_party/nimble")

if(NOT EXISTS "${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/include/host/ble_hs.h")
  message(FATAL_ERROR "RA8_USE_NIMBLE=ON but Apache NimBLE vendor tree is missing at "
                      "${_RA8_NIMBLE_VENDOR_DIR}."
  )
endif()

# Surface the upstream public include dirs through an INTERFACE
# library so apps can `#include "host/ble_hs.h"` /
# `#include "nimble/transport.h"` / `#include "nimble/nimble_npl.h"`
# without the per-app CMakeLists having to enumerate the path list.
add_library(nimble INTERFACE)

# Apps that link `nimble` are by definition the cross-compiled target
# build, so any TU that pulls in NimBLE headers via this interface
# library should also see RA8_TARGET_BUILD. The host unit-test build in
# tests/CMakeLists.txt does not link `nimble`, so it stays unaffected
# and the wrappers fall through to their portable bookkeeping path.
target_compile_definitions(nimble INTERFACE RA8_TARGET_BUILD)

target_include_directories(
  nimble SYSTEM
  INTERFACE ${_RA8_NIMBLE_VENDOR_DIR}/nimble/include
            ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/include
            ${_RA8_NIMBLE_VENDOR_DIR}/nimble/host/mesh/include
            ${_RA8_NIMBLE_VENDOR_DIR}/nimble/transport/include
            ${_RA8_NIMBLE_VENDOR_DIR}/porting/nimble/include
            # Upstream ``nimble/nimble_npl.h`` chains to ``nimble/nimble_npl_os.h``,
            # which every port supplies. Ours is
            # ``port/nimble/inc/nimble/nimble_npl_os.h``, so point the whole build
            # at it. This used to fall back to the upstream "dummy" port's header
            # for translation units that reference NimBLE host APIs without going
            # through nimble_port_threadx (e.g. application TUs that call the
            # NimBLE host APIs directly).
            # That put two different definitions of ``struct ble_npl_mutex`` and
            # friends into one image depending on which target a TU linked, which
            # is an ABI mismatch no diagnostic reports.
            ${_RA8_NIMBLE_REPO_ROOT}/port/nimble/inc
)

# NimBLE's NPL header pulls ThreadX primitives in via our
# port/nimble/inc/nimble_npl_threadx.h shim. Inherit the ThreadX include
# dirs from the `threadx` target rather than hard-coding them.
target_link_libraries(nimble INTERFACE threadx)

# Pull in our ra8_ble <-> NimBLE transport adapter and the ThreadX-
# backed NPL shim. Defines the `nimble_port_threadx` library that
# carries `ble_hci_ra8_ble.c` and `nimble_npl_threadx.c`.
add_subdirectory(${_RA8_NIMBLE_REPO_ROOT}/port/nimble ${CMAKE_BINARY_DIR}/port_nimble)

message(STATUS "NimBLE enabled: ${_RA8_NIMBLE_VENDOR_DIR}")
