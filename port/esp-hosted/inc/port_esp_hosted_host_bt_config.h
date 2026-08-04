/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/inc/port_esp_hosted_host_bt_config.h
 * @brief esp-hosted port header: which Bluetooth host runs above HCI.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * One of the ten port headers the vendored esp-hosted core includes by
 * name (see ``docs/SOUP/esp-hosted-host.md``). The esp-hosted HCI drivers
 * (``host/drivers/bt/vhci_drv.c`` and ``hci_stub_drv.c``) are written for
 * two possible Bluetooth hosts and select between them here.
 *
 * This project already vendors Apache NimBLE
 * (``libs/third_party/nimble/``) with a first-party ThreadX port under
 * ``port/nimble/``, so NimBLE is the host and Bluedroid is off. That is a
 * fact about this tree, not a preference: no Bluedroid source exists here,
 * and selecting it would leave the HCI driver referencing
 * ``esp_bluedroid_hci.h`` -- a header the tree does not contain.
 *
 * ``H_BT_ENABLE_LL_INIT`` is off for the same structural reason. It asks
 * the *host* to bring up a local link-layer controller before opening HCI;
 * on this board the controller is the ESP32-C6 across the SPI link and it
 * initialises itself, so a host-side link-layer init would be initialising
 * a controller that is not there.
 *
 * @since 0.1.0
 */

#pragma once

/**
 * @def H_BT_HOST_ESP_NIMBLE
 * @brief Route esp-hosted HCI traffic to the Apache NimBLE host.
 * @details One. NimBLE is vendored at ``libs/third_party/nimble/`` and
 * ported onto ThreadX by ``port/nimble/``, so it is the only Bluetooth
 * host this image can carry.
 * @note Read-only build configuration.
 * @warning Mutually exclusive with ::H_BT_HOST_ESP_BLUEDROID; the vendored
 *          HCI driver compiles both blocks when both are set.
 * @par Example:
 * @code
 * #if H_BT_HOST_ESP_NIMBLE
 * ble_transport_to_hs_evt(evt_buf);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_BT_HOST_ESP_NIMBLE (1)

/**
 * @def H_BT_HOST_ESP_BLUEDROID
 * @brief Route esp-hosted HCI traffic to Espressif's Bluedroid host.
 * @details Zero. Bluedroid is an ESP-IDF component and is not vendored
 * here; selecting it would reference ``esp_bluedroid_hci.h``, which this
 * tree does not contain.
 * @note Read-only build configuration.
 * @warning Mutually exclusive with ::H_BT_HOST_ESP_NIMBLE.
 * @par Example:
 * @code
 * #if H_BT_HOST_ESP_BLUEDROID
 * hosted_hci_bluedroid_send(data, len);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_BT_HOST_ESP_BLUEDROID (0)

/**
 * @def H_BT_ENABLE_LL_INIT
 * @brief Whether the host initialises a local Bluetooth link-layer.
 * @details Zero. The link-layer controller lives on the ESP32-C6 and comes
 * up with the co-processor image; the RA8 only speaks HCI to it over the
 * esp-hosted transport.
 * @note Read-only build configuration.
 * @warning Setting this to 1 makes the vendored VHCI driver call an
 *          on-chip controller init that does not exist on the RA8D2.
 * @par Example:
 * @code
 * #if H_BT_ENABLE_LL_INIT
 * ble_ll_init();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_BT_ENABLE_LL_INIT (0)
