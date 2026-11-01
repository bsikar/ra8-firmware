/**
 * @file ra8_nsc_eth.c
 * @brief NSC veneer: ethernet frame TX / RX
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * The veneers validate every NS pointer/length, then hand the frame to
 * ``ra8_nsc_eth_bind``'s target: the ``ra8_io_netif`` seam when the secure
 * side has bound one, else the ``ra8_net_pal`` singleton, the pre-seam
 * default. Both paths land on the same ``ra8_eth`` driver, so an unbound
 * build behaves exactly as before this seam existed. Adds
 * ``__attribute__((cmse_nonsecure_entry))`` and runtime address checks via
 * ``cmse_check_address_range``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_netif.h"
#include "ra8_net_pal.h"
#include "ra8_nsc.h"
#include "ra8_nsc_veneer.h"

static const char* const s_tag = "NSCETH";

/**
 * @brief The seam target ``ra8_nsc_eth_bind`` installed, or NULL when the
 *   veneers still fall back to the ``ra8_net_pal`` singleton.
 */
static const ra8_io_netif_t* s_netif = nullptr;

/*
 * Weak forward references into ``ra8_io``: the seam's archive is not part
 * of every image's link (same precedent as the board camera adapter's
 * weak ``ra8_io_i2c_bus`` pulls). An unbound veneer never calls these, so
 * an image without ``ra8_io`` still links with the references resolving to
 * NULL; ``ra8_nsc_eth_bind`` refuses to bind when they are unresolved,
 * which cannot happen for a bound call path anyway since binding requires
 * ``ra8_io_netif_bind_net_pal``/``ra8_io_netif_bind_c6link``, both in the
 * same archive that defines these two functions.
 */
extern ra8_err_t ra8_io_netif_send(const ra8_io_netif_t* netif,
                                   const uint8_t* frame,
                                   uint16_t len) __attribute__((weak));
extern ra8_err_t ra8_io_netif_poll(const ra8_io_netif_t* netif,
                                   uint8_t* out_buf,
                                   uint16_t* inout_len) __attribute__((weak));

/**
 * @brief Secure-side wiring: point the ethernet veneers at the
 *   ``ra8_io_netif`` seam.
 *
 * @details Caller owns the handle: bind a backend into it first (see
 *   ``ra8_io_netif_net_pal.h`` / ``ra8_io_netif_c6link.h``), then hand it
 *   here. Passing nullptr unbinds and restores the ``ra8_net_pal`` singleton
 *   fallback. This is NOT an NS-callable veneer: only secure-world init
 *   may rewire it.
 *
 * @param[in] netif The bound seam handle, or nullptr to unbind.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok                  Handle stored.
 * @retval k_ra8_err_not_initialized The ``ra8_io`` archive is absent from
 *   this image's link, so the handle could never be dispatched through
 *   (only reachable when binding non-NULL; unbinding always succeeds).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_nsc_eth_bind(const ra8_io_netif_t* netif)
{
  if (netif == nullptr) {
    s_netif = nullptr;  /* Unbind: restore the ra8_net_pal singleton fallback. */
    return k_ra8_ok;
  }
  if ((ra8_io_netif_send == nullptr) || (ra8_io_netif_poll == nullptr)) {
    return k_ra8_err_not_initialized;
  }
  s_netif = netif;
  return k_ra8_ok;
}

/**
 * @brief NSC veneer: transmit an Ethernet frame from Non-Secure code.
 *
 * @details Validates the NS-side pointer/length, runs cmse_check_address_range
 *   (TZ builds), and forwards through the bound ``ra8_io_netif`` seam,
 *   else to ``ra8_net_pal_send_frame``.
 *
 * @param[in] ns_frame Non-Secure source buffer holding the L2 frame.
 * @param[in] len      Frame length in bytes; 1..k_ra8_nsc_eth_frame_max.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Frame queued for TX.
 * @retval k_ra8_err_null_ptr     ``ns_frame`` was NULL.
 * @retval k_ra8_err_invalid_arg  ``len`` out of range.
 *
 * @pre TrustZone substrate has been initialized.
 * @pre ``ns_frame`` lies entirely within the NS data region.
 * @post On success the secure-side TX path owns a copy of the frame.
 * @post On failure no bytes were forwarded to ra8_net_pal.
 *
 * @note Thread-safe: serialises through the secure ra8_net_pal lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_eth_send(const uint8_t* ns_frame, uint16_t len)
{
  RA8_CHECK_NULL_PTR(ns_frame, s_tag, "eth_send: ns_frame");
  if ((len == 0U) || (len > (uint16_t)k_ra8_nsc_eth_frame_max)) {
    return k_ra8_err_invalid_arg;
  }
  RA8_NSC_CHECK_NS_RANGE_R(ns_frame, len);
  if (s_netif != nullptr) {
    return ra8_io_netif_send(s_netif, ns_frame, len);
  }
  return ra8_net_pal_send_frame(ns_frame, len);
}

/**
 * @brief NSC veneer: receive an Ethernet frame into a Non-Secure buffer.
 *
 * @details Validates capacity and pointer ranges, then forwards to
 *   through the bound ``ra8_io_netif`` seam, else to
 *   ``ra8_net_pal_recv_frame``.
 *
 * @param[out]    ns_buf    Non-Secure destination buffer.
 * @param[in,out] inout_len In: capacity; out: bytes written.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Frame copied; ``*inout_len`` updated.
 * @retval k_ra8_err_null_ptr     A pointer argument was NULL.
 * @retval k_ra8_err_invalid_arg  Capacity below the frame max.
 *
 * @pre TrustZone substrate has been initialized.
 * @pre Both pointers lie in the NS data region.
 * @post On success ``*inout_len`` reflects the byte count copied.
 * @post On failure ``*ns_buf`` may still have been touched.
 *
 * @note Thread-safe: serialises through the secure ra8_net_pal lock.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_eth_recv(uint8_t* ns_buf, uint16_t* inout_len)
{
  RA8_CHECK_NULL_PTR(ns_buf, s_tag, "eth_recv: ns_buf");
  RA8_CHECK_NULL_PTR(inout_len, s_tag, "eth_recv: inout_len");
  /* Range-validate the NS caller's inout_len pointer BEFORE dereferencing it.
   * A hostile NS caller could aim inout_len at Secure memory; the capacity read
   * below would then read Secure state. cmse_check_address_range exists to
   * prevent exactly this deref-before-check ordering (T5-07). */
  RA8_NSC_CHECK_NS_RANGE_RW(inout_len, sizeof(*inout_len));
  if (*inout_len < (uint16_t)k_ra8_nsc_eth_frame_max) {
    return k_ra8_err_invalid_arg;
  }
  RA8_NSC_CHECK_NS_RANGE_RW(ns_buf, *inout_len);
  if (s_netif != nullptr) {
    return ra8_io_netif_poll(s_netif, ns_buf, inout_len);
  }
  return ra8_net_pal_recv_frame(ns_buf, inout_len);
}
