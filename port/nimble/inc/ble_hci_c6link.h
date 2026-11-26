/**
 * @file port/nimble/inc/ble_hci_c6link.h
 * @brief Binds the host-side `ra8_ble` HCI seam onto the `ra8_c6link` SPI link.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Connects `ra8_ble` (`libs/ra8_hal/inc/ra8_ble.h`) to an open ESP32-C6
 * companion-link handle (`libs/ra8_c6link/inc/ra8_c6link.h`) so NimBLE's
 * `ble_hci_ra8_ble.c` adapter and any caller of `ra8_ble` send host->controller
 * HCI command/ACL packets across SPI and receive inbound controller->host H4
 * events/ACL frames during `ra8_ble_dispatch()`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "ra8_c6link.h"
#include "ra8_err.h"

/**
 * @brief Bind `ra8_ble` onto an open `ra8_c6link_t` companion link.
 *
 * @details
 * Attaches the inbound `ESP_HCI_IF` callback via ::ra8_c6link_set_hci_rx and
 * binds transmit/poll callbacks via ::ra8_ble_bind_transport. Does not open or
 * reset `ra8_ble` or @p link; callers open both handles separately.
 *
 * @param[in,out] link Open companion-link handle; must be non-null and remain
 *                     open until ::ble_hci_c6link_deinit is called.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Transport bound cleanly onto @p link.
 * @retval k_ra8_err_null_ptr @p link was null.
 * @retval k_ra8_err_not_initialized @p link is not open.
 *
 * @pre @p link was opened with ::ra8_c6link_open and has no conflicting HCI
 *      receiver registered.
 * @post Outbound `ra8_ble` commands and ACL packets clock over SPI via
 *       ::ra8_c6link_hci_send.
 * @post ::ra8_ble_dispatch polls @p link and delivers inbound H4 packets via
 *       `ra8_ble`'s event/ACL callbacks.
 *
 * @note Not thread-safe; bind from the task or main loop that polls both
 *       `ra8_ble` and `ra8_c6link`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ble_hci_c6link_init(ra8_c6link_t* link);

/**
 * @brief Unbind `ra8_ble` from the companion link and restore loopback mode.
 *
 * @details
 * Detaches the `ra8_ble` external transport and clears the `ESP_HCI_IF` sink
 * on the previously bound `ra8_c6link_t` handle if it is still open.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Unbound cleanly; `ra8_ble` operates in loopback mode again.
 *
 * @post Subsequent `ra8_ble` operations use the built-in loopback buffers.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ble_hci_c6link_deinit(void);

/**
 * @brief Query whether `ra8_ble` is currently bound to a companion link.
 *
 * @return true When ::ble_hci_c6link_init succeeded with no subsequent deinit.
 * @return false Otherwise.
 * @since 0.1.0
 */
[[nodiscard]] bool ble_hci_c6link_is_bound(void);

#ifdef __cplusplus
}
#endif
