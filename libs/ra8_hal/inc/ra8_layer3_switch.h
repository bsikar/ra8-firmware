/**
 * @file ra8_layer3_switch.h
 * @brief Layer-3 Ethernet switch driver -- placeholder
 * @ingroup grp_hal_net
 *
 * @details
 * Mirrors the FSP `r_layer3_switch` API shape (open / route-add /
 * route-delete / status / close). The Layer-3 switch is a forwarding
 * engine attached to the on-chip Ethernet MAC on RZ-class parts and a
 * subset of RA8 networking variants.
 *
 * @warning The Renesas RA8D2 silicon does not carry a Layer-3 switch
 *          block (HUM Ch 1 "Overview" feature matrix lists only
 *          ETHERC + GMAC-style controllers). This file ships a
 *          host-testable placeholder that exposes the FSP-shaped
 *          API and rejects route-table operations with
 *          `k_ra8_err_not_supported`.
 *
 * Reference: FSP `r_layer3_switch` driver shape.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @struct ra8_layer3_switch_cfg_t
 * @brief Configuration descriptor for `ra8_layer3_switch_open`.
 */
typedef struct {
  uint8_t  port_count;  /**< Number of physical ports.          */
  uint16_t mtu_bytes;   /**< Maximum transfer unit per port.    */
  uint8_t  promiscuous; /**< Non-zero to put switch in promisc. */
} ra8_layer3_switch_cfg_t;

/**
 * @struct ra8_layer3_switch_route_t
 * @brief A single forwarding-table entry.
 */
typedef struct {
  uint32_t dst_ip;      /**< Destination IPv4 (network order). */
  uint32_t mask;        /**< Subnet mask.                      */
  uint8_t  egress_port; /**< Egress port index.                */
} ra8_layer3_switch_route_t;

/**
 * @brief Open the Layer-3 switch driver.
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Driver opened.
 * @retval k_ra8_err_null_ptr `cfg` was NULL.
 * @retval k_ra8_err_invalid_arg `port_count` zero or `mtu_bytes` zero.
 * @retval k_ra8_err_exists Driver already opened.
 *
 * @pre Single-threaded init context.
 *
 * @post Driver in open state.
 * @post `ra8_layer3_switch_close` will succeed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_layer3_switch_open(const ra8_layer3_switch_cfg_t* cfg);

/**
 * @brief Add a route to the forwarding table.
 *
 * @param[in] route Route entry. Must not be NULL.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_err_null_ptr `route` was NULL.
 * @retval k_ra8_err_not_initialized Driver was never opened.
 * @retval k_ra8_err_not_supported Placeholder.
 *
 * @pre `ra8_layer3_switch_open` succeeded.
 *
 * @post On real silicon: route installed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_layer3_switch_route_add(const ra8_layer3_switch_route_t* route);

/**
 * @brief Remove a route from the forwarding table.
 *
 * @param[in] dst_ip Destination IPv4 of the route to drop.
 * @param[in] mask   Subnet mask of the route to drop.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_err_not_initialized Driver was never opened.
 * @retval k_ra8_err_not_supported Placeholder.
 *
 * @pre `ra8_layer3_switch_open` succeeded.
 *
 * @post On real silicon: route removed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_layer3_switch_route_delete(uint32_t dst_ip, uint32_t mask);

/**
 * @brief Snapshot the open / promiscuous flags.
 *
 * @param[out] out_open    Receives 1 if open.
 * @param[out] out_promisc Receives 1 if promiscuous mode is active.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Status copied.
 * @retval k_ra8_err_null_ptr Either pointer NULL.
 *
 * @pre Pointers reference writable memory.
 *
 * @post `*out_open` and `*out_promisc` reflect driver state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_layer3_switch_status_get(uint8_t* out_open, uint8_t* out_promisc);

/**
 * @brief Close the Layer-3 switch driver.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Released.
 * @retval k_ra8_err_invalid_state Driver was never opened.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post Subsequent operations return `k_ra8_err_not_initialized`.
 * @post `ra8_layer3_switch_open` may be called again.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_layer3_switch_close(void);

#ifdef __cplusplus
}
#endif
