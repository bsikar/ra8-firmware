/**
 * @file ble_hci_ra_ble.h
 * @brief Apache NimBLE HCI transport adapter against the ra_ble HCI ring
 *
 * @par Tag
 * [Ring 4 / PORT] {World: S}
 *
 * @details
 * Bridges Apache NimBLE's HCI transport entry points onto our
 * ``ra_ble`` driver:
 *
 *   - ``ble_transport_to_ll_cmd(buf)``  -- host -> controller HCI command
 *   - ``ble_transport_to_ll_acl(om)``   -- host -> controller ACL data
 *   - ``ble_transport_to_hs_evt(buf)``  -- controller -> host HCI event
 *   - ``ble_transport_to_hs_acl(om)``   -- controller -> host ACL data
 *
 * The host->controller path is wired to ``ra_ble_hci_send_command`` /
 * ``ra_ble_hci_send_acl_data``. The controller->host path is fed by
 * ``ra_ble_attach_event_handler`` / ``ra_ble_attach_acl_handler``
 * callbacks invoked from ``ra_ble_dispatch`` at every kernel tick (or
 * from the controller IRQ once enabled).
 *
 * The expected lifecycle is:
 *
 *  1. App calls ``ra_ble_init()`` (clocks the radio and opens the HCI
 *     mailbox).
 *  2. App calls ``ble_hci_ra_ble_init()`` -- this file -- to attach
 *     the inbound callbacks and clear any private state.
 *  3. App calls ``nimble_port_init()`` followed by ``ble_hs_sched_start()``
 *     and the host stack starts pumping HCI commands through the
 *     adapter.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/* =============================================================================
 * Public limits
 * =============================================================================
 */

/**
 * @enum ble_hci_ra_ble_limits_t
 * @brief Sizing constants for the adapter's private scratch buffers.
 *
 * @details
 * The adapter copies inbound HCI events / ACL frames out of the
 * driver-owned scratch buffer (``ra_ble_event_fn_t`` / ``ra_ble_acl_fn_t``
 * promise the pointer is only valid for the duration of the callback)
 * into NimBLE-allocated mbufs. The maxima below match the matching
 * ``ra_ble_limits_t`` constants but live here so the adapter does not
 * reach across the driver header boundary for sizing.
 */
typedef enum : uint16_t {
  /** Maximum HCI event parameter byte count + 2-byte header. */
  k_ble_hci_ra_ble_evt_buf_max = 257U,
  /** Maximum HCI ACL payload byte count + 4-byte header. */
  k_ble_hci_ra_ble_acl_buf_max = 255U,
  /** ACL handle field carries the 12-bit handle plus PB/BC flags. */
  k_ble_hci_ra_ble_handle_mask = 0x0FFFU,
} ble_hci_ra_ble_limits_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Attach the adapter to the ra_ble HCI ring.
 *
 * @details
 * Registers ``priv_event_cb`` and ``priv_acl_cb`` with
 * ``ra_ble_attach_event_handler`` / ``ra_ble_attach_acl_handler``.
 * The callbacks copy the inbound bytes out of the driver scratch
 * region into NimBLE-allocated buffers and hand them to
 * ``ble_transport_to_hs_evt`` / ``ble_transport_to_hs_acl``.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Callbacks attached.
 * @retval k_ra_err_not_initialized ra_ble_init has not been called.
 *
 * @pre ``ra_ble_init`` returned ``k_ra_ok``.
 * @pre ``nimble_port_init`` has been called (mbuf pools live).
 * @post Inbound HCI events / ACL frames flow into NimBLE.
 * @post Subsequent ``ble_transport_to_ll_*`` calls are accepted.
 *
 * @note Not thread-safe; call once during system bring-up.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ble_hci_ra_ble_init(void);

/**
 * @brief Detach the adapter from the ra_ble HCI ring.
 *
 * @details
 * Calls ``ra_ble_attach_event_handler(nullptr, nullptr)`` and
 * ``ra_ble_attach_acl_handler(nullptr, nullptr)`` so subsequent
 * inbound traffic is dropped on the floor by the driver.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok Adapter detached.
 *
 * @pre ``ble_hci_ra_ble_init`` returned ``k_ra_ok``.
 * @post Inbound HCI events / ACL frames are silently dropped.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ble_hci_ra_ble_deinit(void);

#ifdef __cplusplus
}
#endif
