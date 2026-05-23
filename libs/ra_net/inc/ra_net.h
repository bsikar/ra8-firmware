/**
 * @file ra_net.h
 * @brief Minimal hand-rolled TCP/IP stack adapter on top of ra_net_pal.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * ``ra_net`` is a tiny, statically-allocated TCP/IPv4 stack that sits
 * on top of the Ring-3 ``ra_eth`` driver via the ``ra_net_pal``
 * descriptor-ring adapter. The goal is _not_ a full third-party TCP/IP
 * port (NetX Duo is the canonical heavy-weight choice; see issue #7).
 * This
 * stack implements the smallest cross-section of the standard sockets
 * surface that supports the apps in this tree:
 *
 *   - Multi-connection TCP echo (RFC 793 segments, simplified FSM).
 *   - UDP datagrams (RFC 768).
 *   - Basic DNS A-record lookup (RFC 1035).
 *   - ARP resolution for the local subnet (RFC 826).
 *   - ICMP echo reply for ping (RFC 792).
 *   - IPv4 only (RFC 791); no fragmentation, no IP options.
 *
 * It is single-threaded. The application must drive it by calling
 * ``ra_net_poll`` from its main loop -- the poll routine drains
 * ``ra_net_pal_recv_frame`` and dispatches to ARP / IPv4 / TCP / UDP
 * handlers, then services any retransmit timers.
 *
 * Resource budget (intentionally bounded -- NASA Power of 10 Rule 3):
 *
 *   - 8 concurrent sockets in total (4 TCP + 4 UDP).
 *   - 4 simultaneous TCP connections per listening socket.
 *   - 4 KB receive buffer + 4 KB send buffer per TCP socket.
 *   - 16-entry ARP cache, 5-minute TTL.
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
 * Sizing constants -- typed enums per project style (no #define magic).
 * =============================================================================
 */

/** @brief Stack-wide capacity constants. */
typedef enum : uint16_t {
  k_ra_net_max_sockets      = 8U,    /**< Total sockets (TCP+UDP). */
  k_ra_net_max_tcp_sockets  = 4U,    /**< Of which TCP. */
  k_ra_net_max_udp_sockets  = 4U,    /**< Of which UDP. */
  k_ra_net_max_tcp_conns    = 8U,    /**< Total tracked TCP conns. */
  k_ra_net_arp_cache_size   = 16U,   /**< ARP cache entry count. */
  k_ra_net_tcp_rx_buf_size  = 4096U, /**< Per-socket TCP RX bytes. */
  k_ra_net_tcp_tx_buf_size  = 4096U, /**< Per-socket TCP TX bytes. */
  k_ra_net_dns_max_hostname = 64U,   /**< DNS query hostname bytes. */
  k_ra_net_invalid_handle   = 0xFFU, /**< Sentinel for unused slot. */
} ra_net_sizes_t;

/** @brief Time / TTL constants in milliseconds. */
typedef enum : uint32_t {
  k_ra_net_arp_ttl_ms     = 300000U, /**< 5 min ARP TTL. */
  k_ra_net_tcp_msl_ms     = 30000U,  /**< Max segment lifetime. */
  k_ra_net_dns_timeout_ms = 2000U,   /**< DNS reply wait. */
} ra_net_times_t;

/* =============================================================================
 * Address types
 * =============================================================================
 */

/**
 * @struct ra_net_ipv4_t
 * @brief IPv4 address stored in network byte order.
 * @details Bytes are A.B.C.D where ``bytes[0]`` is the most significant
 * octet (RFC 791 wire layout). The struct wrapper avoids accidental
 * mixing with raw ``uint32_t`` integers in host order.
 */
typedef struct {
  uint8_t bytes[4]; /**< Network-order octets. */
} ra_net_ipv4_t;

/**
 * @struct ra_net_mac_t
 * @brief 48-bit Ethernet MAC address.
 */
typedef struct {
  uint8_t bytes[6]; /**< Wire-order MAC. */
} ra_net_mac_t;

/**
 * @struct ra_net_config_t
 * @brief Stack bring-up configuration.
 * @details Passed to ::ra_net_open. All fields are mandatory.
 */
typedef struct {
  ra_net_mac_t  mac;        /**< Local MAC address. */
  ra_net_ipv4_t ip;         /**< Local IPv4 address. */
  ra_net_ipv4_t netmask;    /**< Local subnet mask. */
  ra_net_ipv4_t gateway;    /**< Default gateway. */
  ra_net_ipv4_t dns_server; /**< Authoritative DNS resolver. */
} ra_net_config_t;

/**
 * @typedef ra_net_handle_t
 * @brief Opaque socket handle (index into the internal table).
 *
 * @details ``k_ra_net_invalid_handle`` (0xFF) is reserved as a
 * "no socket" sentinel. Real handles are in [0, k_ra_net_max_sockets).
 */
typedef uint8_t ra_net_handle_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the TCP/IP stack on top of ra_net_pal.
 *
 * @details
 * Calls ::ra_net_pal_init internally with ``cfg->mac`` and stores the
 * IP / netmask / gateway / DNS configuration for use by the IPv4
 * forwarding code. Resets all socket and ARP-cache state.
 *
 * Algorithm:
 *   1. Validate ``cfg`` non-NULL.
 *   2. Reset socket table, TCP connection table, ARP cache.
 *   3. Stash the configuration block.
 *   4. Bring up the PAL with the configured MAC.
 *
 * @param[in] cfg Stack configuration. Must be non-NULL.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok Stack opened.
 * @retval k_ra_err_null_ptr ``cfg`` was NULL.
 * @retval k_ra_err_hw_init_failed PAL bring-up failed.
 *
 * @pre ``ra_mstp_init`` and ``ra_pwr_init`` have been called.
 * @pre Stack is currently closed.
 *
 * @post All socket slots are free.
 * @post ARP cache is empty.
 *
 * @note Not thread-safe.
 * @see ra_net_close
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_net_open(const ra_net_config_t* cfg);

/**
 * @brief Tear down the TCP/IP stack.
 *
 * @details
 * Closes all open sockets, flushes the ARP cache, and calls
 * ::ra_net_pal_deinit.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok Stack closed.
 * @retval k_ra_err_invalid_state Stack was not open.
 *
 * @pre Stack has been opened.
 * @post Subsequent socket calls return ``k_ra_err_invalid_state``.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_net_close(void);

/**
 * @brief Drive the stack -- drain RX, run timers, dispatch protocols.
 *
 * @details
 * The application must call this from its main loop. Each call:
 *
 *   1. Drains every pending frame from ``ra_net_pal_recv_frame``.
 *   2. Dispatches by EtherType -- ARP (RFC 826) or IPv4 (RFC 791).
 *   3. For IPv4 frames, dispatches by protocol -- ICMP / TCP / UDP.
 *   4. Runs TCP retransmit + DNS timeout timers.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok Poll completed (zero or more frames processed).
 * @retval k_ra_err_invalid_state Stack not open.
 *
 * @pre Stack is open.
 * @post All ready frames consumed; deferred work scheduled.
 * @note Not thread-safe; not re-entrant.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_net_poll(void);

/* =============================================================================
 * Sockets
 * =============================================================================
 */

/**
 * @brief Create a UDP socket bound to ``local_port``.
 *
 * @param[in]  local_port    Local port (host order). Non-zero.
 * @param[out] out_handle    Receives the socket handle.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok            Socket created.
 * @retval k_ra_err_null_ptr  ``out_handle`` was NULL.
 * @retval k_ra_err_invalid_arg ``local_port`` was zero.
 * @retval k_ra_err_no_mem    Socket table full.
 * @retval k_ra_err_exists    Port already in use.
 * @retval k_ra_err_invalid_state Stack not open.
 *
 * @pre Stack is open.
 * @pre ``out_handle`` non-NULL.
 * @post On success, ``*out_handle`` < k_ra_net_max_sockets.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_net_socket_udp(uint16_t local_port, ra_net_handle_t* out_handle);

/**
 * @brief Create a TCP listening socket on ``local_port``.
 *
 * @param[in]  local_port    Local port (host order).
 * @param[out] out_handle    Receives the listener handle.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok           Listener installed.
 * @retval k_ra_err_null_ptr ``out_handle`` was NULL.
 * @retval k_ra_err_invalid_arg ``local_port`` was zero.
 * @retval k_ra_err_no_mem   Socket table full.
 * @retval k_ra_err_invalid_state Stack not open.
 *
 * @pre Stack is open; ``out_handle`` non-NULL.
 * @post Socket transitions to LISTEN per RFC 793.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_net_socket_tcp_listen(uint16_t local_port, ra_net_handle_t* out_handle);

/**
 * @brief Active-open a TCP connection to ``remote_ip:remote_port``.
 *
 * @details
 * Sends a SYN per RFC 793 and returns immediately; the caller polls
 * ::ra_net_send/::ra_net_recv to discover establishment. The simplified
 * FSM treats SYN-SENT, ESTABLISHED, FIN_WAIT, and CLOSED states.
 *
 * @param[in]  remote_ip    Destination IPv4 address.
 * @param[in]  remote_port  Destination port (host order).
 * @param[out] out_handle   Receives the socket handle.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok           Connect initiated.
 * @retval k_ra_err_null_ptr ``out_handle`` was NULL.
 * @retval k_ra_err_invalid_arg ``remote_port`` was zero.
 * @retval k_ra_err_no_mem   Socket / connection table full.
 * @retval k_ra_err_invalid_state Stack not open.
 *
 * @pre Stack is open; ``out_handle`` non-NULL.
 * @post On success a SYN has been queued for transmit.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_net_socket_tcp_connect(ra_net_ipv4_t    remote_ip,
                                                 uint16_t         remote_port,
                                                 ra_net_handle_t* out_handle);

/**
 * @brief Send a datagram (UDP) or stream bytes (TCP) on ``handle``.
 *
 * @details
 * UDP: ``remote_ip`` and ``remote_port`` are required.
 * TCP: the connection is already bound; ``remote_*`` are ignored
 * (callers may pass zeros).
 *
 * @param[in] handle      Socket handle from ::ra_net_socket_*.
 * @param[in] buf         Payload bytes.
 * @param[in] len         Payload length in bytes; non-zero.
 * @param[in] remote_ip   UDP destination IP (ignored for TCP).
 * @param[in] remote_port UDP destination port (ignored for TCP).
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok           Bytes accepted (UDP -- queued; TCP -- buffered).
 * @retval k_ra_err_null_ptr ``buf`` was NULL.
 * @retval k_ra_err_invalid_arg Bad args.
 * @retval k_ra_err_invalid_state Bad handle / wrong socket state.
 * @retval k_ra_err_no_mem   TX buffer full.
 *
 * @pre Stack is open and handle is valid.
 * @post On success, payload is queued for the wire (UDP) or copied
 *       into the TCP send buffer.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_net_send(ra_net_handle_t handle,
                                   const uint8_t*  buf,
                                   uint16_t        len,
                                   ra_net_ipv4_t   remote_ip,
                                   uint16_t        remote_port);

/**
 * @brief Receive a datagram (UDP) or stream bytes (TCP) from ``handle``.
 *
 * @param[in]  handle        Socket handle.
 * @param[out] buf           Destination buffer.
 * @param[in]  max_len       Capacity of ``buf``.
 * @param[out] got_len       Bytes actually written.
 * @param[out] remote_ip     UDP source IP (TCP: peer IP).
 * @param[out] remote_port   UDP source port (TCP: peer port).
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok           Data delivered.
 * @retval k_ra_err_null_ptr A required pointer was NULL.
 * @retval k_ra_err_invalid_arg Bad arguments.
 * @retval k_ra_err_no_data  Nothing pending.
 * @retval k_ra_err_invalid_state Bad handle / wrong socket state.
 *
 * @pre All output pointers non-NULL.
 * @pre Stack is open and handle is valid.
 * @post On ``k_ra_ok``, ``*got_len`` <= ``max_len``.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_net_recv(ra_net_handle_t handle,
                                   uint8_t*        buf,
                                   uint16_t        max_len,
                                   uint16_t*       got_len,
                                   ra_net_ipv4_t*  remote_ip,
                                   uint16_t*       remote_port);

/**
 * @brief Close ``handle``.
 *
 * @details
 * UDP: free the slot immediately.
 * TCP: send a FIN per RFC 793 and walk through FIN_WAIT -> CLOSED.
 *
 * @param[in] handle Socket handle.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok               Close initiated / completed.
 * @retval k_ra_err_invalid_arg  Bad handle.
 * @retval k_ra_err_invalid_state Stack not open.
 *
 * @pre Stack is open.
 * @post UDP slot is freed; TCP slot is in CLOSED or FIN_WAIT.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_net_close_socket(ra_net_handle_t handle);

/* =============================================================================
 * DNS
 * =============================================================================
 */

/**
 * @brief Issue a blocking-ish DNS A-record lookup against the
 *        configured DNS server.
 *
 * @details
 * Builds an RFC 1035 query (no recursion-helper bit set), transmits
 * via UDP/53, and polls the stack until either a response arrives or
 * ``k_ra_net_dns_timeout_ms`` elapses. The caller is expected to be
 * driving ::ra_net_poll concurrently when running on hardware; the
 * implementation is poll-driven and never blocks the IRQ path.
 *
 * @param[in]  hostname   ASCII hostname, NUL-terminated, <= 64 bytes.
 * @param[out] out_ip     Receives the resolved A-record on success.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok                Resolution succeeded.
 * @retval k_ra_err_null_ptr      ``hostname`` or ``out_ip`` was NULL.
 * @retval k_ra_err_invalid_arg   Hostname too long.
 * @retval k_ra_err_timeout       No response before deadline.
 * @retval k_ra_err_invalid_state Stack not open.
 * @retval k_ra_err_not_found     Server replied NXDOMAIN.
 *
 * @pre Stack is open.
 * @pre ``hostname`` is NUL-terminated.
 * @post On success ``*out_ip`` contains the resolved address.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_net_dns_query(const char* hostname, ra_net_ipv4_t* out_ip);

/* =============================================================================
 * Test/instrumentation hooks (internal but used by tests/test_ra_net.c).
 * =============================================================================
 */

/**
 * @brief Inject a synthetic ethernet frame into the stack RX path.
 *
 * @details
 * Test-only helper. Pushes a frame into the underlying ``ra_net_pal``
 * loopback ring so the next ::ra_net_poll dispatches it through the
 * usual ARP / IP / TCP / UDP code paths. Production callers must not
 * use this -- it returns ``k_ra_err_not_supported`` outside the
 * simulator build.
 *
 * @param[in] frame Pointer to a complete ethernet frame.
 * @param[in] len   Frame length in bytes (>= 14, <= 1518).
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok               Frame queued.
 * @retval k_ra_err_null_ptr     ``frame`` was NULL.
 * @retval k_ra_err_invalid_arg  Length out of range.
 * @retval k_ra_err_not_supported Not a simulator build.
 * @retval k_ra_err_invalid_state Stack not open.
 *
 * @pre Stack is open; simulator build active.
 * @post The frame is queued for the next poll.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_net_test_inject_frame(const uint8_t* frame, uint16_t len);

/**
 * @brief Dequeue the next outbound frame from the PAL TX ring (test only).
 *
 * @details
 * Mirrors ::ra_net_test_inject_frame for the TX direction; lets the
 * test harness inspect the bytes the stack would have put on the wire.
 *
 * @param[out]    out_buf   Destination buffer, >= 1518 bytes.
 * @param[in,out] inout_len On entry: capacity of ``out_buf``.
 *                          On return: bytes written.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok                Frame delivered.
 * @retval k_ra_err_no_data       No outbound frame ready.
 * @retval k_ra_err_null_ptr      Required pointer was NULL.
 * @retval k_ra_err_invalid_state Stack not open.
 *
 * @pre Stack is open.
 * @post On ``k_ra_ok``, ``*inout_len`` reflects actual frame size.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_net_test_drain_tx(uint8_t* out_buf, uint16_t* inout_len);

#ifdef __cplusplus
}
#endif
