/**
 * @file ra8_io_netif.h
 * @brief ra8_io netif facade -- one L2 frame-mover seam over the board's
 * @ingroup grp_io
 *        two link transports.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The board moves 802.3 frames twice, through two transports with the same
 * duty and no shared type: the ESWM Ethernet MAC behind the ::ra8_net_pal
 * singleton and the ESP32-C6 Wi-Fi link (\p ra8_c6link_eth_send over the
 * esp-hosted seam). Two near-identical NetX drivers
 * (\p nx_ether_driver_ra8_eth and \p nx_ether_driver_c6) hard-wire one side
 * each by function name, so switching an app between Ethernet and Wi-Fi is
 * a source edit. This facade gives callers one handle-based frame-mover so
 * the transport an app routes through is a bind-time decision, not a
 * call-site rewrite.
 *
 * Modeled on the ::ra8_io_spi_bus_t pattern: the handle (::ra8_io_netif_t)
 * is caller-allocated; a backend `_bind_*()` helper (see
 * `ra8_io_netif_net_pal.h` and `ra8_io_netif_c6link.h`) fills the opaque
 * vtable with thin trampolines to the existing transports, unmodified. No
 * dynamic allocation occurs and no MMIO happens here -- the wrapped
 * transports carry every hardware citation.
 *
 * The facade moves frames only. Link bring-up and pump cadence stay with
 * the caller: the ESWM side requires the PAL initialized, the C6 side the
 * link open (and ::ra8_io_netif_poll pumps one transaction itself, so a
 * caller that only polls still makes progress). Both are Ring 4 / PAL, so
 * no ring inversion (see `docs/RING_AND_WORLD.md`).
 *
 * @par Example:
 * @code
 * ra8_io_netif_t netif = {};
 * (void)ra8_io_netif_bind_net_pal(&netif);          // today's board
 * // (void)ra8_io_netif_bind_c6link(&netif, ...);   // the Wi-Fi revision
 * (void)ra8_io_netif_send(&netif, frame, len);
 * @endcode
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

#include "ra8_err.h"

/* =============================================================================
 * Interface + handle
 * =============================================================================
 */

/**
 * @struct ra8_io_netif_iface
 * @brief Opaque to applications -- backends implement this internally and
 *        export a binding helper through their own header.
 *
 * @details
 * The concrete vtable layout lives in `src/ra8_io_netif_internal.h`. Apps
 * never construct one; they call a backend's `_bind_*()` helper, which binds
 * the backend's const vtable into a caller-owned ::ra8_io_netif_t.
 *
 * @since 0.1.0
 */
typedef struct ra8_io_netif_iface ra8_io_netif_iface_t;

/**
 * @struct ra8_io_netif_t
 * @brief Caller-allocated L2 frame-mover handle binding a backend to its
 *        context.
 *
 * @details
 * Zero-initialise (`= {}`) and pass to a backend `_bind_*()` helper, which
 * fills the vtable and context. Treat the fields as private; reach the
 * seam only through the `ra8_io_netif_*` functions below. The handle must
 * out-live every call made through it.
 *
 * @invariant `iface` is non-NULL once a backend has been bound.
 *
 * @since 0.1.0
 */
typedef struct {
  const ra8_io_netif_iface_t* iface; /**< Bound backend vtable (private).    */
  void*                       ctx;   /**< Backend-private context (private). */
} ra8_io_netif_t;

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Hand one complete 802.3 frame to the bound transport for transmit.
 *
 * @details
 * Forwards to the bound backend's `send` primitive (::ra8_net_pal_send_frame
 * or ::ra8_c6link_eth_send): the frame bytes, header first, no FCS, are the
 * transport's own contract, and so are its error codes. `len == 0` is a
 * no-op success.
 *
 * @param[in] netif Bound seam handle.
 * @param[in] frame Ethernet frame bytes (header + payload, no FCS); may be
 *                  NULL only when `len == 0`.
 * @param[in] len   Frame length in bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Frame accepted for transmit.
 * @retval k_ra8_err_null_ptr        `netif` was NULL.
 * @retval k_ra8_err_not_initialized No backend is bound to `netif`.
 * @retval ...                       The bound transport's own verdict
 *                                  (::ra8_net_pal_send_frame /
 *                                  ::ra8_c6link_eth_send) passes through
 *                                  unchanged.
 *
 * @pre A backend has been bound into `netif` and the transport behind it is
 *      up (PAL initialized / link open).
 * @post On success the frame was accepted; the caller may drop `frame`.
 * @post On any non-ok return no caller-visible handle state changed.
 *
 * @note Thread safety is the bound transport's own.
 *
 * @see ra8_io_netif_poll  The receive half.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_netif_send(const ra8_io_netif_t* netif, const uint8_t* frame, uint16_t len);

/**
 * @brief Pump the bound transport and pull at most one received frame.
 *
 * @details
 * Forwards to the bound backend's `poll` primitive. The ESWM backend maps
 * it to ::ra8_net_pal_recv_frame (pull); the C6 backend pumps one
 * ::ra8_c6link_poll transaction so its receive callback stages frames, then
 * drains one. When no frame is ready the call returns
 * ::k_ra8_err_no_data so callers can poll without blocking.
 *
 * @param[in]     netif     Bound seam handle.
 * @param[out]    out_buf   Destination buffer for the frame bytes.
 * @param[in,out] inout_len On entry: capacity of `out_buf`. On exit: bytes
 *                          written (success only).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Frame copied into `out_buf`.
 * @retval k_ra8_err_no_data         No frame ready (poll-friendly).
 * @retval k_ra8_err_null_ptr        `netif` / `out_buf` / `inout_len` NULL.
 * @retval k_ra8_err_not_initialized No backend is bound to `netif`.
 * @retval k_ra8_err_invalid_size    The staged frame does not fit `out_buf`
 *                                  (C6 backend; the frame stays staged).
 * @retval ...                       The bound transport's own verdict (a
 *                                  full TX ring, a dead link) passes
 *                                  through unchanged.
 *
 * @pre A backend has been bound into `netif`.
 * @pre The ESWM side requires the PAL initialized.
 * @post On success `*inout_len` holds the frame length actually written.
 *
 * @note Thread safety is the bound transport's own; the C6 backend's
 *       staging ring is not protected against concurrent pollers.
 *
 * @see ra8_io_netif_send  The transmit half.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_netif_poll(const ra8_io_netif_t* netif, uint8_t* out_buf, uint16_t* inout_len);

#ifdef __cplusplus
}
#endif
