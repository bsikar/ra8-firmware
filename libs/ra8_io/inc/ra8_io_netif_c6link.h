/**
 * @file ra8_io_netif_c6link.h
 * @brief ESP32-C6 Wi-Fi backend binder for the ra8_io netif facade.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Binds a caller-owned ::ra8_io_netif_t to an open ESP32-C6 link
 * (::ra8_c6link_t, unmodified) through thin trampolines to
 * ::ra8_c6link_eth_send / ::ra8_c6link_poll. The link must already be open
 * -- the facade owns frame movement only, never bring-up.
 *
 * The C6 delivers received frames through the link's ::ra8_c6link_rx_cb_t
 * callback, which ::ra8_c6link_open fixed at open time, so the backend
 * cannot install one at bind time. Instead it exports
 * ::ra8_io_netif_c6link_rx_sink for the app to name as the link's
 * `rx_cb` in its ::ra8_c6link_cfg, with the caller-allocated
 * ::ra8_io_netif_c6link_ctx_t as `cb_ctx`. The sink stages frames into a
 * caller-supplied ring; ::ra8_io_netif_poll then pumps one
 * ::ra8_c6link_poll transaction (which runs the sink) and drains one
 * staged frame, so a caller that only polls still makes progress.
 *
 * @par Example:
 * @code
 * static ra8_io_netif_c6link_ctx_t c6_ctx;
 * static uint8_t c6_ring[4 * k_ra8_io_netif_c6link_slot_bytes];
 *
 * ra8_c6link_cfg_t cfg = { .transport = seam, .arena = buf,
 *                          .arena_bytes = sizeof buf,
 *                          .rx_cb = ra8_io_netif_c6link_rx_sink,
 *                          .cb_ctx = &c6_ctx };
 * (void)ra8_c6link_open(&link, &cfg);
 * ra8_io_netif_t netif = {};
 * (void)ra8_io_netif_bind_c6link(&netif, &c6_ctx, &link, c6_ring,
 *                                (uint16_t)sizeof c6_ring);
 * @endcode
 *
 * @note The link config shares `cb_ctx` between `event_cb` and `rx_cb`;
 *       an app that wants both sinks needs its own dispatch on top.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_c6link.h"
#include "ra8_err.h"
#include "ra8_io_netif.h"

/**
 * @brief One staging ring slot: 2-byte little-endian length prefix, then
 *        the frame bytes.
 *
 * @details
 * The C6 forwards at most ::k_ra8_c6link_max_payload frame bytes per
 * frame, so a slot of ::k_ra8_c6link_max_payload + 2 covers any frame the
 * link can deliver. Callers size their ring as
 * `slots * ::k_ra8_io_netif_c6link_slot_bytes`.
 *
 * @since 0.1.0
 */
#define k_ra8_io_netif_c6link_slot_bytes (k_ra8_c6link_max_payload + 2U)

/**
 * @struct ra8_io_netif_c6link_ctx_t
 * @brief Caller-allocated backend context for the C6 netif backend.
 *
 * @details
 * Zero-initialise and pass to ::ra8_io_netif_bind_c6link. Treat the fields
 * as private; the backend fills and owns them from bind on. The struct
 * must out-live every call made through the bound handle, and it is the
 * `cb_ctx` the link's `rx_cb` receives.
 *
 * @since 0.1.0
 */
typedef struct {
  struct ra8_c6link* link;   /**< Bound open link (drives send and poll).    */
  uint8_t*           ring;   /**< Caller-supplied staging ring.              */
  uint16_t           slots;  /**< Ring capacity in frames (bind computes).   */
  uint16_t           head;   /**< Index of the next frame to drain (private).*/
  uint16_t           count;  /**< Frames staged and not yet drained (private).*/
  uint16_t           dropped;/**< Frames dropped when the ring was full.     */
} ra8_io_netif_c6link_ctx_t;

/**
 * @brief Link `rx_cb` that stages frames for the C6 netif backend.
 *
 * @details
 * Name this as ::ra8_c6link_cfg::rx_cb with the backend struct as
 * `cb_ctx` when the link is configured. The sink validates the frame
 * (non-NULL, non-zero, at most ::k_ra8_c6link_max_payload bytes), stages
 * it into the context's ring, and silently counts a drop when the ring is
 * full -- the co-processor send path has no back-pressure, so a full ring
 * must never block ::ra8_c6link_poll. It must not be called from any
 * other thread while the link pumps, per the c6link callback contract.
 *
 * @param[in] cb_ctx The backend struct (::ra8_io_netif_c6link_ctx_t).
 * @param[in] frame  Frame bytes, destination address first; valid only
 *                   for the duration of the call.
 * @param[in] len    Frame length in bytes.
 *
 * @return Nothing.
 *
 * @since 0.1.0
 */
void ra8_io_netif_c6link_rx_sink(void* cb_ctx, const uint8_t* frame, uint16_t len);

/**
 * @brief Bind an open ESP32-C6 link into a netif handle.
 *
 * @details
 * Fills `netif` with the C6 backend vtable whose trampolines forward
 * ::ra8_io_netif_send to ::ra8_c6link_eth_send on the bound link and
 * ::ra8_io_netif_poll to one ::ra8_c6link_poll transaction plus a drain
 * of the next staged frame. The ring is carved into
 * ::k_ra8_io_netif_c6link_slot_bytes slots; at least one slot must fit.
 *
 * @param[out] netif      Caller-allocated handle; zero-initialised.
 * @param[out] backend    Caller-allocated backend context.
 * @param[in]  link       Open link handle. A NULL link is accepted at
 *                        bind time (bind before ::ra8_c6link_open): send
 *                        then reports the transport's NULL verdict and
 *                        poll skips the pump, reports ::k_ra8_err_no_data,
 *                        until the caller re-binds with the open link.
 * @param[in]  ring       Ring storage, `ring_bytes` long.
 * @param[in]  ring_bytes Ring size in bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Backend bound.
 * @retval k_ra8_err_null_ptr    Any pointer argument was NULL.
 * @retval k_ra8_err_invalid_arg `ring_bytes` holds fewer than one slot.
 *
 * @pre `netif` and `backend` are zero-initialised.
 * @post On success the backend's counters are reset and every
 *       `ra8_io_netif_*` call routes to the bound link until the next
 *       bind.
 *
 * @see ra8_io_netif_bind_net_pal  The ESWM twin.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_netif_bind_c6link(ra8_io_netif_t*            netif,
                                                 ra8_io_netif_c6link_ctx_t* backend,
                                                 struct ra8_c6link*         link,
                                                 uint8_t*                   ring,
                                                 uint16_t                   ring_bytes);

#ifdef __cplusplus
}
#endif
