/**
 * @file port/esp-hosted/inc/port_esp_hosted_host_sdio.h
 * @brief esp-hosted port header: SDIO transport constants.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * One of the ten port headers the vendored esp-hosted core includes by
 * name (see ``docs/SOUP/esp-hosted-host.md``). Transport selection happens
 * *inside shared vendored files*, so this header must exist and must
 * define its two symbols even though this project selects
 * ``H_TRANSPORT_SPI``: ``host/drivers/transport/transport_drv.h`` picks
 * the matching header with a preprocessor chain, and the SDIO driver TU is
 * still parsed by tooling that walks the vendored tree.
 *
 * The values are the upstream defaults for the SDIO transport, taken from
 * the vendored ``common/transport/esp_hosted_transport.h``
 * (``ESP_TRANSPORT_SDIO_MAX_BUF_SIZE``) and from the SDIO driver's own
 * unresponsive-peripheral sentinel. They are correct rather than merely
 * present: if the SDIO transport is ever selected on this board they are
 * the values it needs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
 * @brief Bytes moved in one SDIO block transfer.
 * @details Derived from the vendored ``ESP_TRANSPORT_SDIO_MAX_BUF_SIZE``
 * (1536), which is the SDIO transport's own frame size and is deliberately
 * smaller than the SPI one.
 * @note Read-only build configuration.
 * @warning Only meaningful when ``H_TRANSPORT_IN_USE == H_TRANSPORT_SDIO``.
 * @par Example:
 * @code
 * uint8_t *buf = malloc(MAX_TRANSPORT_BUFFER_SIZE);
 * @endcode
 * @since 0.1.0
 */
#define MAX_TRANSPORT_BUFFER_SIZE (ESP_TRANSPORT_SDIO_MAX_BUF_SIZE)

/**
 * @def ESP_HOSTED_SDIO_UNRESPONSIVE_CODE
 * @brief All-ones word returned when the SDIO co-processor does not answer.
 * @details The SDIO driver reads peripheral registers over the bus; a card
 * that has stopped clocking reads back as every bit set. The driver
 * compares against this sentinel to tell "register value 0xFFFFFFFF" from
 * "no answer at all" and restarts the transport.
 * @note Read-only build configuration.
 * @warning A genuine register whose value is all-ones is indistinguishable
 *          from an unresponsive peripheral; upstream accepts that.
 * @par Example:
 * @code
 * if (reg == ESP_HOSTED_SDIO_UNRESPONSIVE_CODE) { restart_transport(); }
 * @endcode
 * @since 0.1.0
 */
#define ESP_HOSTED_SDIO_UNRESPONSIVE_CODE (0xFFFFFFFFU)
