# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/esp_hosted.cmake
#
# Top-level integration of the vendored Espressif esp-hosted-mcu host
# driver (libs/third_party/esp-hosted/, pinned 949bb30; see
# docs/SOUP/esp-hosted-host.md). Exposes the `RA8_USE_ESP_HOSTED`
# option; when ON, this file:
#
#   1. Verifies that ThreadX is also enabled -- the OS-abstraction
#      vtable the driver calls through is implemented on TX_QUEUE /
#      TX_SEMAPHORE / TX_MUTEX / TX_THREAD / TX_TIMER / TX_BYTE_POOL.
#   2. Compiles the SPI-transport half of the vendored driver into the
#      `esp_hosted_objs` object library, surfaced through the
#      `esp_hosted` interface library.
#   3. Pulls in port/esp-hosted/, which supplies the ten port headers the
#      driver includes by name, the ESP-IDF compatibility headers, and
#      the first-party implementations behind the vtable.
#
# Apps that want the C6 link declare `USES esp_hosted` in their
# ra8_add_app() call; the include dirs and the ThreadX dependency flow
# through the interface.
#
# ---------------------------------------------------------------------
# WHICH VENDORED TRANSLATION UNITS ARE COMPILED, AND WHY NOT THE REST
# ---------------------------------------------------------------------
# The list below is the SPI transport, the serial channel, the RPC wire
# codec and the shared utilities -- every vendored file that can be
# compiled against a first-party port alone.
#
# Everything above that line stops at one dependency: `esp_wifi.h`.
# `transport_drv.c`, the whole `drivers/rpc/` layer and the public
# `esp_hosted_api.c` are written against ESP-IDF's Wi-Fi API and name 43
# distinct `wifi_*_t` / `esp_netif_*` types in their declarations, which
# this tree does not have.
#
# An earlier version of this comment said those layouts "are what the
# co-processor decodes on the far side of the link". That is NOT true, and
# #490 disproved it on the bench before `libs/ra8_c6link` was written. The
# C6 decodes PROTOBUF: `grep -c 'wifi_config_t\|esp_netif'` over
# `common/proto/esp_hosted_rpc.pb-c.{h,c}` returns 0, `WifiStaConfig` is a
# message with named fields, and upstream's own `rpc_req.c` converts into
# it field by field. Padding, field order and struct size on this side
# never reach the co-processor at all.
#
# So reproducing those 43 types would buy ESP-IDF *source* compatibility
# and nothing else. `libs/ra8_c6link` speaks the same wire through the
# generated codec compiled below, in about 1500 lines, with no ESP-IDF
# type surface -- and `examples/ek_ra8d2/hw_validated/c6/c6_wifi_link`
# takes the co-processor's Wi-Fi station up through it on silicon. These
# files stay excluded because they are upstream's API, not because the
# capability is missing.
#
# Three further exclusions are structural rather than staged:
#
#   * `common/mempool/mempool*.c` -- `mempool_ll.h` includes
#     freertos/FreeRTOS.h, portmacro.h, task.h and semphr.h
#     unconditionally. It is a FreeRTOS data structure. `H_USE_MEMPOOL`
#     is left undefined in the port config for exactly this reason, and
#     the port's own fixed ThreadX byte pool provides the bounded,
#     allocate-once behaviour the upstream pool existed to provide.
#   * `drivers/serial/serial_drv.c` and
#     `drivers/virtual_serial_if/serial_if.c` -- the two vendored files
#     that expand `HOSTED_CALLOC`, whose failure arm is a `goto` to a
#     caller-supplied label. Supplying that macro would put a `goto` in
#     first-party code, which NASA Power of 10 Rule 1 forbids and
#     `check_no_goto_setjmp.py` rejects with no allowlist. Substituting a
#     `return` for the jump is not an option either: `serial_drv.c`'s
#     `free_bufs` label frees two buffers before returning, so a bare
#     `return` would leak both. The macro is therefore absent -- the
#     reasoning is written out in
#     `port/esp-hosted/inc/port_esp_hosted_host_os.h` -- and these two
#     files go with it. That costs nothing today: both are control-plane
#     TLV helpers whose consumers are in the RPC layer, which does not
#     compile in this build anyway. When the RPC layer is brought in, they
#     need a jump-free allocate-or-bail path.
#   * `drivers/transport/{sdio,spi_hd,uart}/` and `drivers/bt/` -- the
#     first three are transports this board does not carry (the C6 is
#     reached over full-duplex SPI only); the Bluetooth pair needs the
#     NimBLE transport headers and belongs with the NimBLE integration.
#
#

# Idempotency guard. A per-app standalone build and the aggregate repo
# build can both reach this file; only the first include should run.
if(DEFINED _RA8_ESP_HOSTED_INCLUDED)
  return()
endif()
set(_RA8_ESP_HOSTED_INCLUDED TRUE)

option(RA8_USE_ESP_HOSTED "Enable the vendored esp-hosted-mcu host driver" OFF)

if(NOT RA8_USE_ESP_HOSTED)
  return()
endif()

# The OS-abstraction vtable is implemented on ThreadX primitives, so the
# build must also pull in ThreadX. Surfacing this as an explicit error
# beats letting the linker complain about missing `_tx_*` symbols.
if(NOT RA8_USE_THREADX)
  message(
    FATAL_ERROR
      "RA8_USE_ESP_HOSTED=ON requires RA8_USE_THREADX=ON. The esp-hosted "
      "OS-abstraction vtable is implemented on TX_QUEUE / TX_SEMAPHORE / "
      "TX_MUTEX / TX_THREAD / TX_TIMER / TX_BYTE_POOL. Enable both options "
      "(or include cmake/threadx.cmake before cmake/esp_hosted.cmake)."
  )
endif()

# Resolve the repo root so this file works whether it is included from
# the top-level CMakeLists.txt or from a standalone per-app build.
get_filename_component(_RA8_ESP_HOSTED_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_RA8_ESP_HOSTED_VENDOR_DIR "${_RA8_ESP_HOSTED_REPO_ROOT}/libs/third_party/esp-hosted")
set(_RA8_ESP_HOSTED_PORT_DIR "${_RA8_ESP_HOSTED_REPO_ROOT}/port/esp-hosted")

if(NOT EXISTS "${_RA8_ESP_HOSTED_VENDOR_DIR}/host/esp_hosted_os_abstraction.h")
  message(FATAL_ERROR "RA8_USE_ESP_HOSTED=ON but the esp-hosted vendor tree is missing at "
                      "${_RA8_ESP_HOSTED_VENDOR_DIR}."
  )
endif()

# Include directories every consumer needs: the port headers first (the
# vendored core includes `port_esp_hosted_host_*.h` by name and must find
# OURS), then the ESP-IDF compatibility headers, then the vendored tree.
set(_RA8_ESP_HOSTED_INCLUDE_DIRS
    ${_RA8_ESP_HOSTED_PORT_DIR}/inc
    ${_RA8_ESP_HOSTED_PORT_DIR}/inc/idf_compat
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/host
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/api/include
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/api/priv
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/drivers/transport
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/drivers/transport/spi
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/drivers/transport/sdio
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/drivers/serial
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/drivers/virtual_serial_if
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/drivers/bt
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/drivers/power_save
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/drivers/rpc/core
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/drivers/rpc/slaveif # LEGACY-OK: upstream directory name
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/drivers/rpc/wrap
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/utils
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/common
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/common/log
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/common/mempool/include
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/common/proto
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/common/protobuf-c
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/common/rpc
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/common/transport
    ${_RA8_ESP_HOSTED_VENDOR_DIR}/common/utils
)

add_library(
  esp_hosted_objs OBJECT
  ${_RA8_ESP_HOSTED_VENDOR_DIR}/common/proto/esp_hosted_rpc.pb-c.c
  ${_RA8_ESP_HOSTED_VENDOR_DIR}/common/protobuf-c/protobuf-c/protobuf-c.c
  ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/api/src/esp_hosted_transport_config.c
  ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/drivers/power_save/power_save_drv.c
  ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/drivers/serial/serial_ll_if.c
  ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/drivers/transport/spi/spi_drv.c
  ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/drivers/transport/transport_util.c
  ${_RA8_ESP_HOSTED_VENDOR_DIR}/host/utils/stats.c
)

# The vendored translation units reach `port_esp_hosted_host_config.h`,
# which derives its GPIO map from `ra8_esp_hosted_pins.h`, which in turn
# names board-layer enumerators rather than re-encoding pin numbers. So the
# board connector header and ra8_core are on the path of every consumer of
# the port headers, vendored ones included -- that is a consequence of
# keeping the pin map in one place, not an accident.
set(_RA8_ESP_HOSTED_FIRST_PARTY_INCLUDE_DIRS
    ${_RA8_ESP_HOSTED_REPO_ROOT}/libs/ra8_core/inc
    ${_RA8_ESP_HOSTED_REPO_ROOT}/libs/ra8_board_ek_ra8d2/inc
)

target_include_directories(
  esp_hosted_objs SYSTEM PUBLIC ${_RA8_ESP_HOSTED_INCLUDE_DIRS}
                                ${_RA8_ESP_HOSTED_FIRST_PARTY_INCLUDE_DIRS}
)

# Vendored SOUP is exempt from the project's warning bar: it is upstream's
# code, held to upstream's standards, and re-flagging it would produce
# noise nobody may act on without forking the vendor tree. Matches the
# `-w` posture cmake/mbedtls.cmake applies to its own object library.
target_compile_options(esp_hosted_objs PRIVATE -w)

target_link_libraries(esp_hosted_objs PUBLIC threadx)

add_library(esp_hosted INTERFACE)

target_include_directories(
  esp_hosted SYSTEM INTERFACE ${_RA8_ESP_HOSTED_INCLUDE_DIRS}
                              ${_RA8_ESP_HOSTED_FIRST_PARTY_INCLUDE_DIRS}
)

target_sources(esp_hosted INTERFACE $<TARGET_OBJECTS:esp_hosted_objs>)

target_link_libraries(esp_hosted INTERFACE threadx)

# Pull in the first-party port: the ten port headers, the ESP-IDF
# compatibility headers, and the implementations behind the vtable.
# Defines `esp_hosted_port_ra8_spi` and the RA8_ESP_HOSTED_PORT_SOURCES
# global property the per-app build splices into its own target.
add_subdirectory(${_RA8_ESP_HOSTED_PORT_DIR} ${CMAKE_BINARY_DIR}/port_esp_hosted)

message(STATUS "esp-hosted enabled: ${_RA8_ESP_HOSTED_VENDOR_DIR}")
