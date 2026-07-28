#
# port/esp-hosted/CMakeLists.txt
#
# Defines the `esp_hosted_port_ra8_spi` interface library, the RA8D2 +
# ThreadX port of the vendored esp-hosted-mcu host driver.
#
# The port has two halves and both live under inc/:
#
#   * The ten `port_esp_hosted_host_*.h` headers the vendored core
#     includes BY NAME. Upstream ships them under host/port/, which this
#     project excludes (docs/SOUP/esp-hosted-host.md); ours replace them
#     and must therefore keep the upstream file names and the upstream
#     macro spellings exactly.
#   * inc/idf_compat/, the ESP-IDF headers the core includes by name and
#     that an ESP-IDF host would get from the SDK -- esp_err.h,
#     esp_log.h, esp_event_base.h, esp_heap_caps.h, esp_mac.h,
#     esp_idf_version.h, sdkconfig.h, endian.h, esp_check.h.
#
# src/ holds the implementations behind the 72-entry OS-abstraction
# vtable, split by slice so each file stays testable on its own:
#
#   * ra8_esp_hosted_rtos.c   -- threads, delays and time over ThreadX,
#     ra8_esp_hosted_rtos_pool.c -- the fixed byte pool and the aligned /
#                                resizable allocation layouts built on it,
#     ra8_esp_hosted_rtos_sync.c -- queues, semaphores and mutexes.
#   * ra8_esp_hosted_gpio.c   -- side-band pins and ICU edges,
#     ra8_esp_hosted_gpio_edge.c -- the software edge detector for pins
#                                the package routes to no ICU channel.
#   * ra8_esp_hosted_spi.c    -- the full-duplex transaction over the
#                                ra8_io SPI bus facade (SCI2 Simple-SPI).
#   * ra8_esp_hosted_osi.c    -- the vtable itself, plus event posting
#                                and the log, restart and power-save rows.
#   * ra8_esp_hosted_osi_absent.c -- the rows for transports this board
#                                does not carry.
#   * ra8_esp_hosted_fmt.c    -- the bounded, heap-free printf subset the
#                                log bridge formats with.
#   * ra8_esp_hosted_log.c    -- the bridge onto ra8_log.
#   * ra8_esp_hosted_pins.c   -- the pin-to-ICU-channel routing table.
#   * ra8_esp_hosted_port.c   -- bring-up and teardown.
#
# Pulled in by cmake/esp_hosted.cmake when RA8_USE_ESP_HOSTED=ON. Apps
# link `esp_hosted_port_ra8_spi`; it brings its own globbed ra8_hal
# sources, so this shim surfaces the include dirs plus the source list as
# a GLOBAL property the per-app CMakeLists splices into its own
# add_executable() call -- the same pattern port/nimble uses.
#
# RUNTIME STATUS: the port compiles, links and is host-tested, but no
# code in it has ever run on silicon. The wire it drives is proven --
# the landed probe examples/ek_ra8d2/hw_validated/c6/c6_spi_probe qualified
# every J26 hole on 2026-07-27 and brought the raw link up at SPI mode 3
# / 1 MHz -- but that probe drives the SCI directly and reaches none of
# this code. Every app that links this port therefore stays under
# examples/*/hw_pending/ until a run on real hardware promotes it.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Header-only interface for source consumers that prefer to compile the
# .c themselves, mirroring the rest of the per-app glob style.
add_library(esp_hosted_port_ra8_spi INTERFACE)

# The port's own sources reach the ra8_io SPI bus facade for the physical
# transport, so its include dir travels with the port rather than being
# something every consuming app has to remember.
get_filename_component(_RA8_ESP_HOSTED_PORT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

target_include_directories(
  esp_hosted_port_ra8_spi
  INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${CMAKE_CURRENT_SOURCE_DIR}/inc/idf_compat
            ${CMAKE_CURRENT_SOURCE_DIR}/src ${_RA8_ESP_HOSTED_PORT_REPO_ROOT}/libs/ra8_io/inc
)

target_link_libraries(esp_hosted_port_ra8_spi INTERFACE esp_hosted threadx)

# Track the source list as a property so apps can splice it into their
# own add_executable() globs. Same pattern as port/nimble and port/filex.
set_property(
  GLOBAL
  PROPERTY RA8_ESP_HOSTED_PORT_SOURCES
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ra8_esp_hosted_fmt.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ra8_esp_hosted_gpio.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ra8_esp_hosted_gpio_edge.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ra8_esp_hosted_log.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ra8_esp_hosted_osi.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ra8_esp_hosted_osi_absent.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ra8_esp_hosted_pins.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ra8_esp_hosted_port.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ra8_esp_hosted_rtos.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ra8_esp_hosted_rtos_pool.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ra8_esp_hosted_rtos_sync.c
           ${CMAKE_CURRENT_SOURCE_DIR}/src/ra8_esp_hosted_spi.c
           # The ra8_io SPI bus facade the transport slice drives. Named here
           # rather than pulled in with `LIBS ra8_io` because that glob would
           # also bring the block-device and file-system layers, which no
           # esp-hosted consumer needs.
           ${_RA8_ESP_HOSTED_PORT_REPO_ROOT}/libs/ra8_io/src/ra8_io_spi_bus.c
           ${_RA8_ESP_HOSTED_PORT_REPO_ROOT}/libs/ra8_io/src/ra8_io_spi_bus_sci_spi.c
)
