/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/inc/port_esp_hosted_host_spi_hd.h
 * @brief esp-hosted port header: half-duplex SPI transport buffer size.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * One of the ten port headers the vendored esp-hosted core includes by
 * name (see ``docs/SOUP/esp-hosted-host.md``). This project selects the
 * full-duplex SPI transport, but transport selection is a preprocessor
 * chain evaluated inside shared vendored files, so the half-duplex header
 * must exist with its upstream name and its one upstream macro.
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
 * @brief Bytes moved in one half-duplex SPI transaction.
 * @details Derived from the vendored ``ESP_TRANSPORT_SPI_HD_MAX_BUF_SIZE``
 * (1600). Half-duplex splits a transaction into command, address and data
 * phases, so unlike the full-duplex transport the buffer bounds one
 * direction at a time rather than both at once.
 * @note Read-only build configuration.
 * @warning Only meaningful when ``H_TRANSPORT_IN_USE == H_TRANSPORT_SPI_HD``.
 * @par Example:
 * @code
 * uint8_t *buf = malloc(MAX_TRANSPORT_BUFFER_SIZE);
 * @endcode
 * @since 0.1.0
 */
#define MAX_TRANSPORT_BUFFER_SIZE (ESP_TRANSPORT_SPI_HD_MAX_BUF_SIZE)
