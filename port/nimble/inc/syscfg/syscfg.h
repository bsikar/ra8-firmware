/**
 * @file port/nimble/inc/syscfg/syscfg.h
 * @brief NimBLE syscfg overrides for the RA8 + ThreadX port.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: S}
 *
 * @details
 * Apache NimBLE translation units include ``"syscfg/syscfg.h"``. Putting this
 * directory ahead of upstream ``porting/nimble/include`` lets this header
 * pre-define RA8-specific overrides before pulling in the upstream defaults
 * via ``#include_next "syscfg/syscfg.h"`` (which wraps each default in
 * ``#ifndef``).
 *
 * Key overrides:
 *   - ``MYNEWT_VAL_BLE_TRANSPORT_EVT_SIZE (257)`` -- matches full 2-byte HCI
 *     event header + 255-byte parameter payload copied by ``ble_hci_ra8_ble``.
 *   - ``MYNEWT_VAL_BLE_TRANSPORT_LL__custom (1)`` and
 *     ``MYNEWT_VAL_BLE_TRANSPORT_LL__socket (0)`` -- route host->controller
 *     traffic through ``ble_hci_ra8_ble`` / ``ble_hci_c6link``.
 *   - ``MYNEWT_VAL_BLE_TRANSPORT_ISO_FROM_HS_COUNT (0)`` /
 *     ``MYNEWT_VAL_BLE_TRANSPORT_ISO_FROM_LL_COUNT (0)`` -- omit unused ISO
 *     pools on static SRAM targets.
 *   - ``MYNEWT_VAL_BLE_SM_LEGACY (0)`` / ``MYNEWT_VAL_BLE_SM_SC (0)`` /
 *     ``MYNEWT_VAL_BLE_MESH (0)`` -- omit SMP crypto and Mesh stacks so host
 *     peripheral targets link without mbedTLS or Mesh sources.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.2.0
 */

#pragma once

/* ----------------------------------------------------------------------- */
/* HCI Transport buffers and routing */
/* ----------------------------------------------------------------------- */

/** @brief Max HCI event octets (2B hdr + 255B params). */
#define MYNEWT_VAL_BLE_TRANSPORT_EVT_SIZE (257)
/** @brief Event buffers in the transport pool. */
#define MYNEWT_VAL_BLE_TRANSPORT_EVT_COUNT (8)
/** @brief Discardable event buffers (advertising reports). */
#define MYNEWT_VAL_BLE_TRANSPORT_EVT_DISCARDABLE_COUNT (8)
/** @brief Max HCI ACL payload octets matching ra8_ble. */
#define MYNEWT_VAL_BLE_TRANSPORT_ACL_SIZE (251)
/** @brief Host-to-controller ACL buffers. */
#define MYNEWT_VAL_BLE_TRANSPORT_ACL_FROM_HS_COUNT (8)
/** @brief Controller-to-host ACL buffers. */
#define MYNEWT_VAL_BLE_TRANSPORT_ACL_FROM_LL_COUNT (8)
/** @brief Unused ISO buffer count. */
#define MYNEWT_VAL_BLE_TRANSPORT_ISO_COUNT (0)
/** @brief Unused ISO host-to-controller count. */
#define MYNEWT_VAL_BLE_TRANSPORT_ISO_FROM_HS_COUNT (0)
/** @brief Unused ISO controller-to-host count. */
#define MYNEWT_VAL_BLE_TRANSPORT_ISO_FROM_LL_COUNT (0)
/** @brief Native host transport binding. */
#define MYNEWT_VAL_BLE_TRANSPORT_HS__native (1)
/** @brief Disable default Linux socket LL transport. */
#define MYNEWT_VAL_BLE_TRANSPORT_LL__socket (0)
/** @brief Select first-party LL transport implementation. */
#define MYNEWT_VAL_BLE_TRANSPORT_LL__custom (1)

/* ----------------------------------------------------------------------- */
/* Host stack roles, store, SMP, and Mesh */
/* ----------------------------------------------------------------------- */

/** @brief Auto-schedule host sync from ble_hs_init(). */
#define MYNEWT_VAL_BLE_HS_AUTO_START (1)
/** @brief Host executes on an active RTOS kernel. */
#define MYNEWT_VAL_BLE_HS_REQUIRE_OS (1)
/** @brief Peripheral role enabled. */
#define MYNEWT_VAL_BLE_ROLE_PERIPHERAL (1)
/** @brief Broadcaster role enabled. */
#define MYNEWT_VAL_BLE_ROLE_BROADCASTER (1)
/** @brief Central role disabled for peripheral images. */
#define MYNEWT_VAL_BLE_ROLE_CENTRAL (0)
/** @brief Observer role disabled for peripheral images. */
#define MYNEWT_VAL_BLE_ROLE_OBSERVER (0)
/** @brief Legacy SMP disabled cleanly. */
#define MYNEWT_VAL_BLE_SM_LEGACY (0)
/** @brief LE Secure Connections SMP disabled cleanly. */
#define MYNEWT_VAL_BLE_SM_SC (0)
/** @brief Bonding disabled cleanly. */
#define MYNEWT_VAL_BLE_SM_BONDING (0)
/** @brief Minimum 1 slot required by upstream ble_store_config_delete_obj when CCCDs enabled. */
#define MYNEWT_VAL_BLE_STORE_MAX_BONDS (1)
/** @brief In-RAM CCCD persistence slots per connection. */
#define MYNEWT_VAL_BLE_STORE_MAX_CCCDS (8)
/** @brief Bluetooth Mesh disabled. */
#define MYNEWT_VAL_BLE_MESH (0)

/* ----------------------------------------------------------------------- */
/* Default GATT GAP / BAS service configuration */
/* ----------------------------------------------------------------------- */

/** @brief Default GAP device name. */
#define MYNEWT_VAL_BLE_SVC_GAP_DEVICE_NAME "EK-RA8D2"
/** @brief Enable notifications on Battery Level characteristic (0x2A19). */
#define MYNEWT_VAL_BLE_SVC_BAS_BATTERY_LEVEL_NOTIFY_ENABLE (1)
/** @brief Allow unauthenticated reads of Battery Level characteristic. */
#define MYNEWT_VAL_BLE_SVC_BAS_BATTERY_LEVEL_READ_PERM (0)

/* Inherit upstream NimBLE defaults for every macro not overridden above. */
#include_next "syscfg/syscfg.h"
