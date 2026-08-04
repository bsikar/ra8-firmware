/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/inc/port_esp_hosted_host_openthread.h
 * @brief esp-hosted port header: OpenThread host support switch.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * One of the ten port headers the vendored esp-hosted core includes by
 * name (see ``docs/SOUP/esp-hosted-host.md``). It carries a single switch,
 * ``H_HOST_OT_ENABLE``, read by ``host/api/include/esp_hosted_openthread.h``
 * and by the RPC event path.
 *
 * The switch is OFF here. The ESP32-C6 silicon does carry an 802.15.4
 * radio and its firmware can expose an OpenThread RCP, but this project
 * uses the co-processor for Wi-Fi and Bluetooth only: nothing in the tree
 * consumes a Thread network interface, and the C6 image built from
 * ``coprocessor/esp32c6/`` is not configured as an RCP. Turning the switch
 * on would make the core reference ``esp_hosted_openthread_get_radio_config``
 * -- a link-time symbol this port deliberately does not define, because
 * defining it would mean shipping a radio configuration no bench has ever
 * exercised.
 *
 * @since 0.1.0
 */

#pragma once

/**
 * @def H_HOST_OT_ENABLE
 * @brief Whether the host drives an OpenThread RCP over esp-hosted.
 * @details Zero. See the file-level rationale: the C6 image is built for
 * Wi-Fi and Bluetooth, and no first-party module consumes Thread.
 * @note Read-only build configuration.
 * @warning Setting this to 1 introduces an undefined reference to
 *          ``esp_hosted_openthread_get_radio_config``; the C6 image must
 *          be rebuilt as an RCP and the port must gain that symbol first.
 * @par Example:
 * @code
 * #if H_HOST_OT_ENABLE
 * esp_hosted_openthread_init();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_HOST_OT_ENABLE (0)
