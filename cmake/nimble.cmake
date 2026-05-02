#
# cmake/nimble.cmake
#
# Top-level integration of the vendored Apache NimBLE host stack on
# RA8D2. Exposes the `RA_USE_NIMBLE` option; when ON, this file:
#
#   1. Verifies that ThreadX is also enabled (the NimBLE Native
#      Porting Layer maps onto ThreadX TX_MUTEX / TX_SEMAPHORE /
#      TX_QUEUE / TX_TIMER primitives).
#   2. Pulls in our `port/nimble/` adapter which:
#        - Implements the NPL primitives on top of ThreadX.
#        - Bridges NimBLE's HCI transport entry points
#          (ble_transport_to_ll_cmd / ble_transport_to_ll_acl) onto
#          our `ra_ble` HCI ring (ra_ble_hci_send_command /
#          ra_ble_hci_send_acl_data) and pumps inbound HCI events /
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
if(DEFINED _RA_NIMBLE_INCLUDED)
    return()
endif()
set(_RA_NIMBLE_INCLUDED TRUE)

option(RA_USE_NIMBLE "Enable the vendored Apache NimBLE host stack" OFF)

if(NOT RA_USE_NIMBLE)
    return()
endif()

# NimBLE's Native Porting Layer (NPL) is implemented on top of
# ThreadX TX_MUTEX / TX_SEMAPHORE / TX_QUEUE / TX_TIMER primitives,
# so the build must also pull in ThreadX. The companion
# `cmake/threadx.cmake` is owned by Wave 13; if it has not landed
# yet the NimBLE build cannot succeed and we surface a clear error
# rather than letting the linker complain about missing `_tx_*`
# symbols.
if(NOT RA_USE_THREADX)
    message(FATAL_ERROR
        "RA_USE_NIMBLE=ON requires RA_USE_THREADX=ON. The NimBLE NPL "
        "is implemented on top of TX_MUTEX / TX_SEMAPHORE / TX_QUEUE / "
        "TX_TIMER. Enable both options (or include cmake/threadx.cmake "
        "before cmake/nimble.cmake).")
endif()

# Resolve the repo root so this file works whether it is included
# from the top-level CMakeLists.txt or from a standalone per-app build.
get_filename_component(_RA_NIMBLE_REPO_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_RA_NIMBLE_VENDOR_DIR "${_RA_NIMBLE_REPO_ROOT}/libs/third_party/nimble")

if(NOT EXISTS "${_RA_NIMBLE_VENDOR_DIR}/nimble/host/include/host/ble_hs.h")
    message(FATAL_ERROR
        "RA_USE_NIMBLE=ON but Apache NimBLE vendor tree is missing at "
        "${_RA_NIMBLE_VENDOR_DIR}.")
endif()

# Surface the upstream public include dirs through an INTERFACE
# library so apps can `#include "host/ble_hs.h"` /
# `#include "nimble/transport.h"` / `#include "nimble/nimble_npl.h"`
# without the per-app CMakeLists having to enumerate the path list.
add_library(nimble INTERFACE)

# Apps that link `nimble` are by definition the cross-compiled target
# build, so any TU that pulls in NimBLE headers via this interface
# library should also see RA_TARGET_BUILD. The host unit-test build in
# tests/CMakeLists.txt does not link `nimble`, so it stays unaffected
# and the wrappers fall through to their portable bookkeeping path.
target_compile_definitions(nimble INTERFACE RA_TARGET_BUILD)

target_include_directories(nimble SYSTEM INTERFACE
    ${_RA_NIMBLE_VENDOR_DIR}/nimble/include
    ${_RA_NIMBLE_VENDOR_DIR}/nimble/host/include
    ${_RA_NIMBLE_VENDOR_DIR}/nimble/host/mesh/include
    ${_RA_NIMBLE_VENDOR_DIR}/nimble/transport/include
    ${_RA_NIMBLE_VENDOR_DIR}/porting/nimble/include
    # Upstream ``nimble/nimble_npl.h`` chains to ``nimble/nimble_npl_os.h``
    # which is provided per-port. Our ThreadX NPL adapter
    # (``port/nimble/nimble_npl_threadx.h``) does not yet ship that shim,
    # so the upstream "dummy" port's header is used to satisfy the chain
    # for translation-units that only reference NimBLE host APIs by
    # function name (e.g. libs/ra_ble_host/src/ra_ble_*.c). Code paths
    # that actually exercise NPL primitives go through
    # nimble_port_threadx, which carries its own ThreadX-typed NPL.
    ${_RA_NIMBLE_VENDOR_DIR}/porting/npl/dummy/include
)

# Apps that link `nimble` get RA_TARGET_BUILD so the ra_ble_host
# wrapper TUs (ra_ble_security.c / ra_ble_gatt_client.c / ra_ble_mesh.c)
# compile in the direct NimBLE call paths. Host unit tests do not link
# `nimble`, so RA_TARGET_BUILD stays undefined and the wrappers fall
# back to their portable bookkeeping-only implementations.
target_compile_definitions(nimble INTERFACE RA_TARGET_BUILD)

# NimBLE's NPL header pulls ThreadX primitives in via our
# port/nimble/nimble_npl_threadx.h shim. Inherit the ThreadX include
# dirs from the `threadx` target rather than hard-coding them.
target_link_libraries(nimble INTERFACE threadx)

# Pull in our ra_ble <-> NimBLE transport adapter and the ThreadX-
# backed NPL shim. Defines the `nimble_port_threadx` library that
# carries `ble_hci_ra_ble.c` and `nimble_npl_threadx.c`.
add_subdirectory(${_RA_NIMBLE_REPO_ROOT}/port/nimble
                 ${CMAKE_BINARY_DIR}/port_nimble)

message(STATUS "NimBLE enabled: ${_RA_NIMBLE_VENDOR_DIR}")
