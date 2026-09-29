/**
 * @file examples/ek_ra8d2/hw_validated/c6/wifi_hal_join/src/wifi_hal_ip.c
 * @brief The NetX Duo DHCP provider the ra8_wifi facade calls for an address.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Implements ::wifi_hal_ip_bind, the ::ra8_wifi_ip_bind_fn. Once ``ra8_wifi``
 * reports the station associated, the facade calls this to turn the L2 link
 * into an IP address.
 *
 * @par What this file used to be
 * The ninety lines of vendor API every application carrying this hook wrote for
 * itself: a packet pool, an IP instance, an ARP cache and a helper-thread stack
 * created by hand, ARP / UDP / ICMP enabled one call at a time, the vendored
 * DHCP client run to a bound lease, and four addresses read back out. That body
 * now lives once, in ``port/netxduo``, and this file is the application's half
 * of it: the buffers, the sizes, and the one line that names the provider.
 *
 * The facade still cannot own the bring-up without dragging NetX Duo into every
 * consumer, which is why the provider sits behind ``ra8_ipif_wifi.h`` rather
 * than inside ``ra8_wifi``. What changed is that the application no longer
 * writes the bring-up, only configures it. The buffers stay file-scope statics
 * because this image has no heap (NASA Rule 3).
 *
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_c6link.h"
#include "ra8_err.h"
#include "ra8_ipif.h"
#include "ra8_ipif_wifi.h"
#include "ra8_wifi.h"
#include "wifi_hal_join.h"

/**
 * @enum wifi_hal_net_size_t
 * @brief Static sizing for the NetX Duo objects this application owns.
 * @details Sized for full Ethernet frames plus the DHCP working set, with a
 *          comfortable packet count so a DHCP retransmit never starves ARP.
 *          These are the figures the hand-written bring-up used; the bring-up
 *          moved, the budget did not.
 * @invariant ::k_wifi_hal_pkt_payload is at least a full Ethernet frame plus the
 *            driver's two-octet alignment slide, so it is at least
 *            ::k_ra8_ipif_pkt_payload_min.
 * @invariant ::k_wifi_hal_pool_bytes holds several ::k_wifi_hal_pkt_payload
 *            packets.
 * @par Example:
 * @code
 * static uint8_t pool[k_wifi_hal_pool_bytes];
 * @endcode
 * @see wifi_hal_ip_bind
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_wifi_hal_pkt_payload = 1568U,  /**< Per-packet payload, in octets.          */
  k_wifi_hal_pool_bytes  = 40960U, /**< Packet-pool backing store, in octets.   */
  k_wifi_hal_ip_stack    = 2048U,  /**< NetX IP helper-thread stack, in octets. */
  k_wifi_hal_arp_bytes   = 1040U,  /**< ARP cache backing store, in octets.     */
  k_wifi_hal_ip_prio     = 3U,     /**< NetX IP helper-thread priority.         */
} wifi_hal_net_size_t;

static_assert((uint32_t)k_wifi_hal_pkt_payload >= (uint32_t)k_ra8_ipif_pkt_payload_min,
              "the application packet payload still clears the facade's floor");

/** @brief Packet-pool backing store. @since 0.1.0 */
alignas(4) static uint8_t s_pool_mem[k_wifi_hal_pool_bytes];
/** @brief NetX IP helper-thread stack. @since 0.1.0 */
alignas(8) static uint8_t s_ip_stack[k_wifi_hal_ip_stack];
/** @brief ARP cache backing store. @since 0.1.0 */
alignas(4) static uint8_t s_arp_cache[k_wifi_hal_arp_bytes];
/** @brief The NetX control blocks and object names the bring-up fills. @since 0.1.0 */
static ra8_ipif_t s_ipif;
/** @brief Context the provider is handed as its ``ip_ctx``. @since 0.1.0 */
static ra8_ipif_wifi_t s_bind;

/** @brief Buffers, sizes and waits this application brings the interface up with.
 *  @details `driver` is left null, which is how ::ra8_ipif_wifi_bind is told to
 *           use the C6 link driver. @since 0.1.0 */
static const ra8_ipif_cfg_t k_ipif_cfg = {
  .name           = "wifi_hal",
  .driver         = nullptr,
  .pool_mem       = s_pool_mem,
  .pool_bytes     = (uint32_t)sizeof(s_pool_mem),
  .pkt_payload    = (uint32_t)k_wifi_hal_pkt_payload,
  .ip_stack       = s_ip_stack,
  .ip_stack_bytes = (uint32_t)sizeof(s_ip_stack),
  .ip_prio        = (uint32_t)k_wifi_hal_ip_prio,
  .arp_cache      = s_arp_cache,
  .arp_bytes      = (uint32_t)sizeof(s_arp_cache),
  .enable_tcp     = false,
  .dhcp_wait_ms   = (uint32_t)k_wifi_hal_dhcp_wait_ms,
};

ra8_err_t wifi_hal_ip_bind(void* ip_ctx, const ra8_wifi_mac_t* mac, ra8_wifi_lease_t* out)
{
  if ((ip_ctx == nullptr) || (mac == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  s_bind = (ra8_ipif_wifi_t){
    .ipif = &s_ipif,
    .cfg  = &k_ipif_cfg,
    .link = (ra8_c6link_t*)ip_ctx,
  };
  /* The handle is file-scope, so a second association would otherwise meet an
     interface still up from the first. Down on a handle that was never up, or
     is already down, is a no-op. */
  (void)ra8_ipif_down(&s_ipif);
  const ra8_err_t leased = ra8_ipif_wifi_bind(&s_bind, mac, out);
  if (leased != k_ra8_ok) {
    return leased;
  }
  out->bound = (out->ip != 0U);
  return k_ra8_ok;
}
