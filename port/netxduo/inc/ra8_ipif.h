/**
 * @file port/netxduo/inc/ra8_ipif.h
 * @brief The NetX Duo IP-interface facade: one bring-up call instead of ninety
 *        lines of vendor API per application.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * ``ra8_wifi`` walks the radio to associated and then hands the application a
 * hole shaped like an ::ra8_wifi_ip_bind_fn: the application must itself create
 * a packet pool, create an IP instance, enable ARP / UDP / ICMP (and TCP), run
 * the DHCP client to a bound lease and read four addresses back out. Three
 * applications in this tree carry that body in near-verbatim copies. This is the
 * shared implementation of it.
 *
 * @par Why this lives in ``port/netxduo/`` and not in ``libs/ra8_wifi``
 * Every declaration below names a NetX Duo type, so a consumer that includes
 * this header links NetX Duo. ``ra8_wifi`` deliberately does not: its facade
 * stays stack-free and host-testable, which is the whole reason the bind hook is
 * a function pointer in the first place. Keeping the provider on the far side of
 * that seam is what lets an application take the radio facade without the IP
 * stack coming with it, the same split ``ra8_io_bus`` already makes.
 *
 * @par The L2 the IP instance runs on
 * ::ra8_ipif_cfg_t carries the NetX Duo link-driver entry point directly, so
 * ``nx_ether_driver_c6`` (Wi-Fi over the ESP32-C6) and
 * ``nx_ether_driver_ra8_eth`` (the on-chip MAC) are already a one-field
 * decision. The issue proposes an ``ra8_netif_t`` L2 seam here instead; that
 * seam is not on ``dev`` yet, and naming the driver NetX itself takes needs
 * nothing that does not exist today.
 *
 * @par Storage
 * Every byte is caller-provided. ::ra8_ipif_t holds the NetX control blocks and
 * the instance names; the pool, IP-thread stack and ARP cache are buffers the
 * caller passes in ::ra8_ipif_cfg_t. Nothing here allocates (NASA Power of 10
 * Rule 3).
 *
 * @par Threading
 * Not thread-safe against itself. ::ra8_ipif_up, ::ra8_ipif_dhcp and
 * ::ra8_ipif_down run in that order on one application thread, exactly as the
 * bodies they replace did.
 *
 * @par Example:
 * @code
 * static ra8_ipif_t s_ipif;
 * alignas(4) static uint8_t s_pool_mem[40960];
 * alignas(8) static uint8_t s_ip_stack[2048];
 * alignas(4) static uint8_t s_arp[1040];
 *
 * const ra8_ipif_cfg_t cfg = {
 *   .name           = "wifi_hal",
 *   .driver         = nx_ether_driver_c6,
 *   .pool_mem       = s_pool_mem,  .pool_bytes     = sizeof(s_pool_mem),
 *   .pkt_payload    = 1568U,
 *   .ip_stack       = s_ip_stack,  .ip_stack_bytes = sizeof(s_ip_stack),
 *   .ip_prio        = 3U,
 *   .arp_cache      = s_arp,       .arp_bytes      = sizeof(s_arp),
 *   .enable_tcp     = false,
 *   .dhcp_wait_ms   = 20000U,
 * };
 *
 * ra8_wifi_lease_t lease = {};
 * if (ra8_ipif_up(&s_ipif, &cfg) == k_ra8_ok) {
 *   (void)ra8_ipif_dhcp(&s_ipif, &lease);
 * }
 * @endcode
 *
 * @see nx_ether_driver_c6.h  The Wi-Fi link driver this most often runs on
 * @see nx_ether_driver_ra8_eth.h  The on-chip twin
 * @see ra8_wifi.h  The radio facade whose bind hook this fills
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

#include "nx_api.h"
#include "nxd_dhcp_client.h"
#include "ra8_err.h"
#include "ra8_wifi.h"

/**
 * @enum ra8_ipif_limit_t
 * @brief The bounds this facade enforces on a caller's configuration.
 *
 * @details
 * ::k_ra8_ipif_name_max is the room ::ra8_ipif_t reserves for one NetX object
 * name; NetX Duo stores the pointer it is given and never copies, so the names
 * have to outlive the objects and therefore live in the caller's handle.
 * ::k_ra8_ipif_pkt_payload_min is a full 1514-octet Ethernet frame plus the
 * two-octet alignment slide the RA8 link drivers apply, rounded to four.
 *
 * @invariant ::k_ra8_ipif_name_max leaves room for the longest suffix this
 *            facade appends plus its terminator.
 *
 * @par Example:
 * @code
 * static_assert(k_ra8_ipif_pkt_payload_min <= 1568U, "stock apps still fit");
 * @endcode
 *
 * @see ra8_ipif_cfg_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_ipif_name_max        = 32U,   /**< Octets reserved per NetX name.    */
  k_ra8_ipif_suffix_max      = 6U,    /**< Octets the longest suffix needs.  */
  k_ra8_ipif_pkt_payload_min = 1520U, /**< Smallest accepted packet payload. */
} ra8_ipif_limit_t;

/**
 * @struct ra8_ipif_cfg
 * @brief What the application supplies: one link driver and four buffers.
 *
 * @details
 * The five sizing fields are the same five every copy of this bring-up already
 * declared as its own private enum; naming them here is what stops the next
 * application inventing a sixth spelling. `name` is optional and only reaches
 * NetX object names, which exist for a debugger and a trace buffer.
 *
 * @invariant `driver` is the entry point ``nx_ip_create`` will be given, so it
 *            must be the driver for a link that is already open and associated.
 * @invariant `pool_bytes` holds at least one `pkt_payload` packet plus NetX
 *            Duo's own per-packet overhead; NetX rejects the pool otherwise.
 * @invariant A non-zero `ip_address` is accompanied by a non-zero `ip_netmask`.
 *
 * @par Addressing
 * Leaving `ip_address` zero is the DHCP case the three Wi-Fi applications use:
 * the instance is created unbound and ::ra8_ipif_dhcp binds it. Setting it (with
 * a mask) is the static case `tls_client`, the TCP echo application and the
 * HTTPS client hand-roll today, and it binds at ::ra8_ipif_up. The two are
 * alternatives, not a sequence.
 *
 * @par Why `disable_udp` reads backwards
 * Every other switch here is positive, but UDP was unconditional before this
 * field existed, and a designated initialiser leaves an unmentioned field zero.
 * A positive `enable_udp` would therefore have turned UDP off under every caller
 * that never heard of it. The negative spelling is what keeps `= {}` meaning
 * what it meant.
 *
 * @par Example:
 * @code
 * ra8_ipif_cfg_t cfg = {.driver = nx_ether_driver_c6};
 * @endcode
 *
 * @see ra8_ipif_up
 * @since 0.1.0
 */
typedef struct ra8_ipif_cfg {
  /** @brief Base for the NetX object names, or null for a default. @since 0.1.0 */
  const char* name;
  /** @brief NetX Duo link-driver entry point ``nx_ip_create`` is given. @since 0.1.0 */
  VOID (*driver)(NX_IP_DRIVER* driver_req);
  void*    pool_mem;       /**< Packet-pool backing store.                           */
  uint32_t pool_bytes;     /**< Octets at `pool_mem`.                                */
  uint32_t pkt_payload;    /**< Per-packet payload, in octets.                       */
  void*    ip_stack;       /**< NetX IP helper-thread stack.                         */
  uint32_t ip_stack_bytes; /**< Octets at `ip_stack`.                                */
  uint32_t ip_prio;        /**< NetX IP helper-thread priority.                      */
  void*    arp_cache;      /**< ARP cache backing store.                             */
  uint32_t arp_bytes;      /**< Octets at `arp_cache`.                               */
  uint32_t ip_address;     /**< Static address in host byte order, or zero for DHCP. */
  uint32_t ip_netmask;     /**< Mask for `ip_address`; ignored when it is zero.      */
  bool     enable_tcp;     /**< Enable TCP as well as ARP, UDP and ICMP.             */
  bool     disable_udp;    /**< Skip ``nx_udp_enable``; UDP is enabled by default.   */
  uint32_t dhcp_wait_ms;   /**< How long ::ra8_ipif_dhcp waits for a lease.          */
} ra8_ipif_cfg_t;

/**
 * @struct ra8_ipif
 * @brief Caller-allocated handle holding the NetX objects this facade creates.
 *
 * @details
 * Zero-initialise it (``= {}``) before the first ::ra8_ipif_up. The fields are
 * public so the application can reach the ``NX_IP`` for a socket call, which is
 * the point: this facade owns the bring-up, not the stack.
 *
 * @invariant `up` is true exactly between a successful ::ra8_ipif_up and the
 *            ::ra8_ipif_down that tears it back down.
 * @invariant `dhcp_started` is true only while a DHCP client exists.
 *
 * @par Example:
 * @code
 * static ra8_ipif_t s_ipif;
 * (void)nx_tcp_socket_create(&s_ipif.ip, &sock, name, ...);
 * @endcode
 *
 * @see ra8_ipif_up
 * @since 0.1.0
 */
typedef struct ra8_ipif {
  NX_PACKET_POOL pool; /**< Packet-pool control block. */
  NX_IP          ip;   /**< IP-instance control block. */
  NX_DHCP        dhcp; /**< DHCP-client control block. */
  /** @brief Packet-pool name; NetX stores this pointer. @since 0.1.0 */
  CHAR pool_name[k_ra8_ipif_name_max];
  /** @brief IP-instance name; NetX stores this pointer. @since 0.1.0 */
  CHAR ip_name[k_ra8_ipif_name_max];
  /** @brief DHCP-client name; NetX stores this pointer. @since 0.1.0 */
  CHAR     dhcp_name[k_ra8_ipif_name_max];
  uint32_t dhcp_wait_ms; /**< Lease wait carried from the configuration. */
  bool     up;           /**< Pool and IP instance exist.                */
  bool     dhcp_started; /**< A DHCP client exists.                      */
} ra8_ipif_t;

/**
 * @brief Create the packet pool and IP instance and enable the protocols.
 *
 * @details
 * Runs the sequence every copy of this bring-up runs: ``nx_system_initialize``,
 * ``nx_packet_pool_create``, ``nx_ip_create`` naming @p cfg->driver, then
 * ``nx_arp_enable``, ``nx_udp_enable`` unless `cfg->disable_udp`, optionally
 * ``nx_tcp_enable``, and ``nx_icmp_enable``. The instance is created on
 * `cfg->ip_address` and `cfg->ip_netmask`, which are zero for the DHCP case.
 * A failure part-way leaves the handle torn back down rather than half-built,
 * so a caller that retries starts from a clean state.
 *
 * @param[out] ipif Handle to bring up; zero-initialised on first use.
 * @param[in]  cfg  Link driver, buffers and sizes.
 *
 * @return ::k_ra8_ok on success.
 * @retval k_ra8_err_null_ptr       @p ipif, @p cfg, `cfg->driver` or any buffer
 *                                  pointer is null.
 * @retval k_ra8_err_invalid_size   A buffer size is zero, or `pkt_payload` is
 *                                  below ::k_ra8_ipif_pkt_payload_min.
 * @retval k_ra8_err_invalid_arg    `ip_address` is set and `ip_netmask` is not.
 * @retval k_ra8_err_invalid_state  @p ipif is already up.
 * @retval k_ra8_err_not_initialized A NetX object could not be created.
 *
 * @pre The L2 link @p cfg->driver bridges onto is open and carrying frames.
 * @post On success `ipif->up` is true and `ipif->ip` is a live NetX IP instance,
 *       bound to `cfg->ip_address` when that is non-zero.
 * @post On failure nothing created by this call survives it.
 *
 * @note Not thread-safe against itself.
 *
 * @par Example:
 * @code
 * if (ra8_ipif_up(&s_ipif, &cfg) != k_ra8_ok) { return; }
 * @endcode
 *
 * @see ra8_ipif_dhcp
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipif_up(ra8_ipif_t* ipif, const ra8_ipif_cfg_t* cfg);

/**
 * @brief Run the DHCP client to a bound lease and read the four addresses out.
 *
 * @details
 * ``nx_dhcp_create`` / ``nx_dhcp_start``, then ``nx_ip_status_check`` for
 * ``NX_IP_ADDRESS_RESOLVED`` bounded by `cfg->dhcp_wait_ms`, then
 * ``nx_ip_address_get``, ``nx_ip_gateway_address_get`` and
 * ``nx_dhcp_server_address_get``. Every address is host byte order, matching
 * ::ra8_wifi_lease_t.
 *
 * @param[in,out] ipif Handle a previous ::ra8_ipif_up brought up.
 * @param[out]    out  Lease to fill; zeroed first, and zeroed again on failure.
 *
 * @return ::k_ra8_ok on a bound lease.
 * @retval k_ra8_err_null_ptr        @p ipif or @p out is null.
 * @retval k_ra8_err_not_initialized @p ipif is not up.
 * @retval k_ra8_err_invalid_state   A DHCP client already exists on @p ipif.
 * @retval k_ra8_err_timeout         No lease inside the configured wait.
 *
 * @pre ::ra8_ipif_up returned ::k_ra8_ok on @p ipif.
 * @post On success `out->bound` is true and `out->ip` is non-zero.
 * @post On failure @p out is all-zero and no DHCP client survives the call.
 *
 * @note Blocks the calling thread for up to the configured wait.
 * @note For an instance ::ra8_ipif_up already bound to a static address there is
 *       no lease to take, so this call does not belong in that path.
 *
 * @par Example:
 * @code
 * ra8_wifi_lease_t lease = {};
 * if (ra8_ipif_dhcp(&s_ipif, &lease) == k_ra8_ok) { use(lease.ip); }
 * @endcode
 *
 * @see ra8_ipif_up
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipif_dhcp(ra8_ipif_t* ipif, ra8_wifi_lease_t* out);

/**
 * @brief Delete every NetX object this facade created, newest first.
 *
 * @details
 * The teardown half none of the three hand-written copies has: DHCP client,
 * then IP instance, then packet pool. Safe on a handle that was never brought
 * up and safe to call twice.
 *
 * @param[in,out] ipif Handle to tear down.
 *
 * @return ::k_ra8_ok when the handle is down, including when it already was.
 * @retval k_ra8_err_null_ptr @p ipif is null.
 * @retval k_ra8_err_busy     A NetX delete refused; the handle is still marked
 *                            down, because leaving it up would strand it.
 *
 * @post `ipif->up` and `ipif->dhcp_started` are false.
 *
 * @note Not thread-safe against itself.
 *
 * @par Example:
 * @code
 * (void)ra8_ipif_down(&s_ipif);
 * @endcode
 *
 * @see ra8_ipif_up
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipif_down(ra8_ipif_t* ipif);

#ifdef __cplusplus
}
#endif
