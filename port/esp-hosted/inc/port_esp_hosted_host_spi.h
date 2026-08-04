/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/inc/port_esp_hosted_host_spi.h
 * @brief esp-hosted port header: full-duplex SPI transport buffer size.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The vendored esp-hosted core includes this header BY NAME from
 * ``host/drivers/transport/transport_drv.h`` whenever
 * ``H_TRANSPORT_IN_USE == H_TRANSPORT_SPI``. Upstream ships it under
 * ``host/port/``, which this project excludes (see
 * ``docs/SOUP/esp-hosted-host.md``); this file is its first-party
 * replacement and must therefore keep the upstream file name and the
 * upstream macro spelling exactly.
 *
 * It supplies exactly one symbol, ``MAX_TRANSPORT_BUFFER_SIZE`` -- the
 * number of bytes clocked in a single full-duplex transaction. Both ends
 * of the link must agree on it or every frame mis-decodes:
 *
 *   - the RA8 host takes it from ``ESP_TRANSPORT_SPI_MAX_BUF_SIZE`` in
 *     the vendored ``common/transport/esp_hosted_transport.h``;
 *   - the ESP32-C6 co-processor image built from
 *     ``coprocessor/esp32c6/`` reports the same 1600 in its sdkconfig;
 *   - the landed link probe ``examples/ek_ra8d2/hw_validated/c6/c6_spi_probe``
 *     hand-decodes frames against its own ``k_c6_proto_buf_size``, also
 *     1600.
 *
 * Deriving it from the vendored constant rather than restating 1600 keeps
 * those three in lockstep: a future upstream bump changes one place.
 *
 * @since 0.1.0
 */

#pragma once

/* Before esp_hosted_transport.h: the vendored header spells uint8_t and
 * uint16_t without including <stdint.h>, relying on an ESP-IDF build having
 * pulled one in earlier. It is SOUP and cannot be edited, so its one
 * first-party includer supplies the dependency instead. */
#include <stdint.h>

#include "esp_hosted_transport.h"

/**
 * @def MAX_TRANSPORT_BUFFER_SIZE
 * @brief Bytes clocked in one full-duplex SPI transaction.
 * @details Derived from the vendored ``ESP_TRANSPORT_SPI_MAX_BUF_SIZE``
 * (1600) so host and co-processor cannot drift apart. Every transmit and
 * receive buffer the SPI driver allocates is exactly this large,
 * regardless of how few payload bytes a frame actually carries.
 * @note Read-only build configuration; consumed by the vendored
 * ``spi_drv.c`` through ``MAX_SPI_BUFFER_SIZE``.
 * @warning Changing this without rebuilding the C6 image breaks the link.
 * @par Example:
 * @code
 * static uint8_t rx_buf[MAX_TRANSPORT_BUFFER_SIZE];
 * @endcode
 * @since 0.1.0
 */
#define MAX_TRANSPORT_BUFFER_SIZE (ESP_TRANSPORT_SPI_MAX_BUF_SIZE)
