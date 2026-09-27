/**
 * @file port/netxduo/inc/ra8_ipif_wifi.h
 * @brief The ::ra8_wifi_ip_bind_fn provider ``ra8_wifi`` promises: one line of
 *        application configuration instead of a hand-written bring-up body.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * ``ra8_wifi.h`` says of its IP hook that "a ready-made provider ships alongside
 * the backend when a stack is available". This is that provider for the NetX
 * Duo stack over the ESP32-C6 station. It is the two lines the three
 * hand-written copies run before their bring-up -- bind the NetX link driver to
 * the open C6 handle, stamp the station MAC on it -- followed by ::ra8_ipif_up
 * and ::ra8_ipif_dhcp. With it, an application's whole IP story is
 * ``cfg.ip_bind = ra8_ipif_wifi_bind; cfg.ip_ctx = &s_wifi_ipif;``.
 *
 * @par Why this is its own translation unit and not part of ``ra8_ipif.c``
 * Every declaration below names either ``nx_ether_driver_c6`` or
 * ``ra8_c6link_t``, so a consumer of this header links the C6 link driver and
 * the C6 facade with it. ::ra8_ipif_up itself names neither: it takes whatever
 * link driver it is given, which is what lets the on-chip MAC use the same
 * bring-up. Keeping the Wi-Fi provider on this side of the seam is the same
 * split one level down that puts ::ra8_ipif in ``port/netxduo`` rather than in
 * ``libs/ra8_wifi``.
 *
 * @par Storage
 * Caller-provided, as everywhere below ::ra8_ipif: ::ra8_ipif_wifi_t holds three
 * pointers and owns none of them. The handle, the configuration and the link all
 * outlive the association (NASA Power of 10 Rule 3).
 *
 * @par Example:
 * @code
 * static ra8_ipif_t      s_ipif;
 * static ra8_ipif_wifi_t s_bind;
 *
 * static const ra8_ipif_cfg_t k_ipif_cfg = {
 *   .name = "wifi_hal", .pool_mem = s_pool_mem, .pool_bytes = sizeof(s_pool_mem),
 *   .pkt_payload = 1568U, .ip_stack = s_ip_stack,
 *   .ip_stack_bytes = sizeof(s_ip_stack), .ip_prio = 3U,
 *   .arp_cache = s_arp, .arp_bytes = sizeof(s_arp), .dhcp_wait_ms = 20000U,
 * };
 *
 * s_bind = (ra8_ipif_wifi_t){.ipif = &s_ipif, .cfg = &k_ipif_cfg, .link = &s_link};
 * wifi_cfg.ip_bind = ra8_ipif_wifi_bind;
 * wifi_cfg.ip_ctx  = &s_bind;
 * @endcode
 *
 * @see ra8_ipif.h  The bring-up this provider drives
 * @see ra8_wifi.h  The radio facade whose hook this fills
 * @see nx_ether_driver_c6.h  The link driver this pairs the bring-up with
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "nx_ether_driver_c6.h"
#include "ra8_c6link.h"
#include "ra8_err.h"
#include "ra8_ipif.h"
#include "ra8_wifi.h"

/**
 * @struct ra8_ipif_wifi
 * @brief What ::ra8_ipif_wifi_bind is handed as its ``ip_ctx``: the handle to
 *        bring up, the configuration to bring it up with, and the link to
 *        bring it up over.
 *
 * @details
 * ``ra8_wifi`` carries a single ``void*`` through to the hook, so everything the
 * provider needs travels in one caller-allocated struct. Nothing here is copied
 * or owned: all three pointers have to outlive the association.
 *
 * @invariant All three members are non-null before the hook runs; the provider
 *            refuses rather than dereferences otherwise.
 * @invariant `cfg->driver` is null, or is ``nx_ether_driver_c6``. Any other
 *            driver would be paired with a C6 bind that does not match it.
 *
 * @par Example:
 * @code
 * ra8_ipif_wifi_t bind = {.ipif = &s_ipif, .cfg = &k_cfg, .link = &s_link};
 * @endcode
 *
 * @see ra8_ipif_wifi_bind
 * @since 0.1.0
 */
typedef struct ra8_ipif_wifi {
  /** @brief Handle ::ra8_ipif_up brings up; zero-initialised. @since 0.1.0 */
  ra8_ipif_t* ipif;
  /** @brief Buffers and sizes; `driver` may be left null. @since 0.1.0 */
  const ra8_ipif_cfg_t* cfg;
  /** @brief Open, associated C6 link the driver bridges onto. @since 0.1.0 */
  ra8_c6link_t* link;
} ra8_ipif_wifi_t;

/**
 * @brief Bring an IP interface up over the associated C6 station and lease it
 *        an address: the ::ra8_wifi_ip_bind_fn ``ra8_wifi`` calls.
 *
 * @details
 * Runs ``nx_ether_driver_c6_bind`` on the link, ``nx_ether_driver_c6_set_mac``
 * with the station address ``ra8_wifi`` read out of the radio, then
 * ::ra8_ipif_up and ::ra8_ipif_dhcp. The MAC has to be stamped before
 * ``nx_ip_create`` fires ``NX_LINK_INITIALIZE``, which is why it is this
 * provider's job and not the application's.
 *
 * When `cfg->driver` is null the C6 driver is used, which is the whole point of
 * this provider; a `cfg->driver` naming anything else is refused rather than
 * silently overwritten, because the C6 bind above it would not match.
 *
 * @param[in]  ip_ctx Pointer to an ::ra8_ipif_wifi_t.
 * @param[in]  mac    Station address to stamp on outgoing frames.
 * @param[out] out    Lease to fill; zeroed first, and zeroed again on failure.
 *
 * @return ::k_ra8_ok on a bound lease.
 * @retval k_ra8_err_null_ptr      @p ip_ctx, @p mac, @p out or any member of
 *                                 the context is null.
 * @retval k_ra8_err_invalid_arg   `cfg->driver` names a driver other than
 *                                 ``nx_ether_driver_c6``.
 * @retval k_ra8_err_invalid_size  A buffer size in `cfg` is rejected.
 * @retval k_ra8_err_invalid_state The handle is already up.
 * @retval k_ra8_err_not_initialized A NetX object could not be created.
 * @retval k_ra8_err_timeout       No lease inside the configured wait.
 *
 * @pre The link is open, its receive callback is ::nx_ether_driver_c6_rx, and
 *      the station is associated.
 * @pre The ThreadX kernel is running.
 * @post On success `out->bound` is true and the handle is up.
 * @post On failure @p out is all-zero and nothing this call created survives
 *       it, so ``ra8_wifi`` retrying the hook starts from a clean handle.
 *
 * @note Blocks the calling thread for up to `cfg->dhcp_wait_ms`.
 * @note Not thread-safe against itself.
 *
 * @par Example:
 * @code
 * cfg.ip_bind = ra8_ipif_wifi_bind;
 * cfg.ip_ctx  = &s_bind;
 * @endcode
 *
 * @see ra8_ipif_up
 * @see ra8_wifi_wait_ip
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipif_wifi_bind(void*                 ip_ctx,
                                           const ra8_wifi_mac_t* mac,
                                           ra8_wifi_lease_t*     out);

#ifdef __cplusplus
}
#endif
