/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/inc/idf_compat/esp_mac.h
 * @brief ESP-IDF MAC-address-type compatibility for the esp-hosted port.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * ``host/esp_hosted_misc.h`` declares three entry points that read and
 * write the co-processor's interface MAC addresses, and each takes an
 * ``esp_mac_type_t`` saying which of the C6's several addresses is meant.
 * That type is an ESP-IDF one, so it is supplied here.
 *
 * The values are not incidental: they cross the SPI link inside RPC
 * requests and are interpreted by the co-processor firmware, which is
 * built from ESP-IDF v5.5.4. Renumbering them would compile cleanly and
 * then address the wrong interface on the far end, so the upstream
 * ordering is reproduced exactly.
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

/**
 * @enum esp_mac_type_t
 * @brief Which of the co-processor's MAC addresses an operation refers to.
 *
 * @details
 * The ESP32-C6 derives several interface addresses from one base address
 * in efuse. The enumerator spellings and ordering are fixed by ESP-IDF and
 * are what the co-processor firmware decodes, so they do not follow the
 * project's ``k_`` prefix convention.
 *
 * @invariant The ordering matches ESP-IDF v5.5.4, which is the version the
 *            co-processor image is built from.
 *
 * @par Example:
 * @code
 * uint8_t mac[6] = {};
 * (void)esp_hosted_iface_mac_addr_get(mac, sizeof(mac), ESP_MAC_WIFI_STA);
 * @endcode
 *
 * @see esp_hosted_iface_mac_addr_get
 * @since 0.1.0
 */
typedef enum : uint8_t {
  // NOLINTBEGIN(readability-identifier-naming) -- ESP-IDF-fixed spellings.
  ESP_MAC_WIFI_STA      = 0U, /**< Wi-Fi station interface.           */
  ESP_MAC_WIFI_SOFTAP   = 1U, /**< Wi-Fi soft access-point interface. */
  ESP_MAC_BT            = 2U, /**< Bluetooth interface.               */
  ESP_MAC_ETH           = 3U, /**< Wired Ethernet interface.          */
  ESP_MAC_IEEE802154    = 4U, /**< 802.15.4 radio, an EUI-64.         */
  ESP_MAC_BASE          = 5U, /**< The efuse base address.            */
  ESP_MAC_EFUSE_FACTORY = 6U, /**< Factory-programmed address.        */
  ESP_MAC_EFUSE_CUSTOM  = 7U, /**< Customer-programmed address.       */
  ESP_MAC_EFUSE_EXT     = 8U, /**< Extension address block.           */
  // NOLINTEND(readability-identifier-naming)
} esp_mac_type_t;
