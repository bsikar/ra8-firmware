/**
 * @file ra_net_ipv4.c
 * @brief IPv4 dispatch (RFC 791) + stack lifecycle for ra_net.
 *
 * @details
 * Holds the singleton stack state and the top-level dispatch loop:
 *
 *   ra_net_open  / ra_net_close      lifecycle
 *   ra_net_poll                      drives the receive path
 *   ra_net_ipv4_send                 helper used by ICMP/UDP/TCP TX
 *
 * Frame format on the wire (IEEE 802.3 + IPv4):
 *
 *     [DA 6][SA 6][ET 2][V4 hdr 20][...payload...]
 *
 * No fragmentation or IP options are produced or accepted -- the stack
 * targets minimal LAN traffic only.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_net.h"
#include "ra_net_internal.h"
#include "ra_net_pal.h"

/* RFC 791 IPv4
 * RFC 826 ARP
 */

/* NOLINTBEGIN(readability-magic-numbers,readability-redundant-casting,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,readability-function-size,readability-identifier-naming) */

/* =============================================================================
 * Singleton state
 * =============================================================================
 */

static ra_net_state_t s_state = {};

#ifdef RA_SIMULATOR_MODE
/* Test-only: side queue for synthetic RX frames so the PAL TX/RX ring
 * stays reserved for stack TX (drained via ra_net_test_drain_tx). */
enum : uint16_t {
  k_inject_q_depth = 8U,
};
typedef struct {
  uint16_t len;
  uint8_t  data[k_ra_net_pal_frame_max];
} ra_net_inject_slot_t;
static ra_net_inject_slot_t s_inject_q[k_inject_q_depth] = {};
static uint16_t             s_inject_head                = 0U;
static uint16_t             s_inject_tail                = 0U;
static uint16_t             s_inject_count               = 0U;
#endif

/**
 * @brief Return the singleton ra_net_state_t pointer.
 *
 * @details See declaration in ra_net_internal.h.
 *
 * @return ra_net_state_t* Pointer to the singleton stack state.
 * @retval !NULL Always returns a valid pointer.
 *
 * @pre Linker has placed s_state in writable storage.
 * @pre Caller is the network thread.
 * @post No state mutation.
 * @post Returned pointer is valid for the program's lifetime.
 *
 * @note Not thread-safe; the underlying state is not lock-guarded.
 *
 * @since 0.1.0
 */
ra_net_state_t* ra_net_internal_state(void)
{
  return &s_state;
}

/* =============================================================================
 * Internet checksum (RFC 1071)
 * =============================================================================
 */

/**
 * @brief Compute the RFC 1071 one's-complement Internet checksum.
 *
 * @details Folds 16-bit words and the trailing odd byte into the
 *          16-bit running total seeded by sum0.
 *
 * @param[in] data Bytes to sum (must not be NULL when len > 0).
 * @param[in] len  Byte count.
 * @param[in] sum0 Initial sum (e.g. pseudo-header for TCP/UDP).
 *
 * @return uint16_t Folded one's-complement checksum.
 * @retval 0xFFFF When the folded sum is zero (RFC 1071 convention).
 * @retval other  Folded one's-complement of the running sum.
 *
 * @pre data is non-NULL when len > 0.
 * @pre Caller is the network thread.
 * @post No state mutation.
 * @post Return value is independent of host byte order.
 *
 * @note Not thread-safe; pure function.
 *
 * @since 0.1.0
 */
uint16_t ra_net_checksum_ones(const uint8_t* data, uint16_t len, uint32_t sum0)
{
  uint32_t sum = sum0;
  for (uint16_t i = 0U; (uint16_t)(i + 1U) < len; i += 2U) {
    sum += (uint32_t)((((uint32_t)data[i]) << 8U) | (uint32_t)data[i + 1U]);
  }
  if ((len & 1U) != 0U) {
    sum += ((uint32_t)data[len - 1U]) << 8U;
  }
  while ((sum >> 16U) != 0U) {
    sum = (sum & 0xFFFFU) + (sum >> 16U);
  }
  return (uint16_t)(~sum & 0xFFFFU);
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Open the ra_net stack.
 *
 * @details Captures cfg, brings the PAL up with the supplied MAC, and
 *          marks the singleton as opened. After this returns k_ra_ok
 *          the application can allocate sockets and call ra_net_poll.
 *
 * @param[in] cfg Stack configuration. Must not be NULL.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Stack opened.
 * @retval k_ra_err_null_ptr        cfg == NULL.
 * @retval k_ra_err_invalid_state   Stack already opened.
 * @retval k_ra_err_hw_init_failed  PAL initialisation failed.
 *
 * @pre cfg points to a valid configuration.
 * @pre Stack is not already opened.
 * @post On success s_state.opened == 1.
 * @post On failure no state is left initialised.
 *
 * @note Not thread-safe; called from init context.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_open(const ra_net_config_t* cfg)
{
  if (cfg == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (s_state.opened != 0U) {
    return k_ra_err_invalid_state;
  }

  (void)memset(&s_state, 0, sizeof(s_state));
  s_state.cfg = *cfg;
#ifdef RA_SIMULATOR_MODE
  s_inject_head  = 0U;
  s_inject_tail  = 0U;
  s_inject_count = 0U;
#endif

  ra_net_pal_mac_t pal_mac = {};
  (void)memcpy(pal_mac.bytes, cfg->mac.bytes, sizeof(pal_mac.bytes));
  ra_err_t err = ra_net_pal_init(&pal_mac);
  if (err != k_ra_ok) {
    return k_ra_err_hw_init_failed;
  }

  s_state.opened = 1U;
  return k_ra_ok;
}

/**
 * @brief Close the ra_net stack.
 *
 * @details Tears down the PAL and zeroes the singleton state.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Stack closed.
 * @retval k_ra_err_invalid_state   Stack was not open.
 *
 * @pre ra_net_open has succeeded.
 * @pre Caller is single-threaded shutdown context.
 * @post Stack is no longer opened.
 * @post All sockets are forgotten.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_close(void)
{
  if (s_state.opened == 0U) {
    return k_ra_err_invalid_state;
  }
  (void)ra_net_pal_deinit();
  (void)memset(&s_state, 0, sizeof(s_state));
  return k_ra_ok;
}

/* =============================================================================
 * IPv4 RX dispatch
 * =============================================================================
 */

/**
 * @brief Dispatch an IPv4 frame to the matching upper-layer handler.
 *
 * @details RFC 791 -- routes by the IPv4 protocol field to ICMP, UDP,
 *          or TCP. Rejects non-v4 packets and packets without a full
 *          IPv4 header.
 *
 * @param[in] frame Ethernet+IPv4 frame bytes. Must not be NULL.
 * @param[in] len   Frame length.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Frame dispatched.
 * @retval k_ra_err_invalid_arg   Frame too short.
 * @retval k_ra_err_not_supported Non-v4 IP header or unknown protocol.
 * @retval other                  Whatever the per-protocol handler returns.
 *
 * @pre ra_net_open has succeeded.
 * @pre frame is non-NULL.
 * @post Per-protocol handler has been invoked.
 * @post Otherwise no state mutation.
 *
 * @note Not thread-safe; called from the network thread.
 *
 * @since 0.1.0
 */
static ra_err_t dispatch_ipv4(const uint8_t* frame, uint16_t len)
{
  if (len < (uint16_t)(k_eth_hdr_len + k_ipv4_hdr_len)) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t* ip = &frame[k_eth_hdr_len];
  if ((ip[0] & 0xF0U) != 0x40U) {
    return k_ra_err_not_supported;
  }
  ra_net_ip_proto_t proto = (ra_net_ip_proto_t)ip[9];
  switch (proto) {
    case k_ip_proto_icmp:
      return ra_net_icmp_handle(frame, len);
    case k_ip_proto_udp:
      return ra_net_udp_handle(frame, len);
    case k_ip_proto_tcp:
      return ra_net_tcp_handle(frame, len);
    default:
      return k_ra_err_not_supported;
  }
}

/**
 * @brief Service inbound traffic.
 *
 * @details Drains the PAL RX ring (or the simulator-only inject queue
 *          when built for unit tests) and dispatches each frame to
 *          ARP or IPv4. Should be called from the application's main
 *          loop.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                 Poll cycle complete.
 * @retval k_ra_err_invalid_state  Stack not opened.
 *
 * @pre ra_net_open has succeeded.
 * @pre Caller is the network thread.
 * @post All currently available inbound frames have been dispatched.
 * @post Stack state may have advanced (cache, sockets, FSMs).
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_poll(void)
{
  if (s_state.opened == 0U) {
    return k_ra_err_invalid_state;
  }

  /* In simulator builds the test harness pushes synthetic frames into
   * a side queue (s_inject_q) before calling poll(); drain that first
   * so the PAL ring stays reserved for outbound frames the test then
   * inspects via ra_net_test_drain_tx(). Production builds skip this
   * block and read straight from the PAL. */
#ifdef RA_SIMULATOR_MODE
  while (s_inject_count != 0U) {
    static uint8_t s_rx[k_ra_net_pal_frame_max];
    uint16_t       rx_len = s_inject_q[s_inject_head].len;
    (void)memcpy(s_rx, s_inject_q[s_inject_head].data, rx_len);
    s_inject_head = (uint16_t)((s_inject_head + 1U) % k_inject_q_depth);
    s_inject_count--;
    if (rx_len < k_eth_hdr_len) {
      continue;
    }
    uint16_t etype = ra_net_be16(&s_rx[12]);
    if (etype == (uint16_t)k_eth_type_arp) {
      (void)ra_net_arp_handle(s_rx, rx_len);
    } else if (etype == (uint16_t)k_eth_type_ipv4) {
      (void)dispatch_ipv4(s_rx, rx_len);
    }
  }
#endif
  /* Drain real PAL frames (production path). Skipped in simulator
   * builds because the PAL ring there exclusively holds outbound
   * frames the test harness wants to inspect. */
#ifndef RA_SIMULATOR_MODE
  for (;;) {
    static uint8_t s_rx[k_ra_net_pal_frame_max];
    uint16_t       rx_len = (uint16_t)k_ra_net_pal_frame_max;
    ra_err_t       e      = ra_net_pal_recv_frame(s_rx, &rx_len);
    if (e != k_ra_ok) {
      break;
    }
    if (rx_len < k_eth_hdr_len) {
      continue;
    }
    uint16_t etype = ra_net_be16(&s_rx[12]);
    if (etype == (uint16_t)k_eth_type_arp) {
      (void)ra_net_arp_handle(s_rx, rx_len);
    } else if (etype == (uint16_t)k_eth_type_ipv4) {
      (void)dispatch_ipv4(s_rx, rx_len);
    } else {
      /* Unknown EtherType -- ignore. */
    }
  }
#endif
  return k_ra_ok;
}

/* =============================================================================
 * IPv4 TX
 * =============================================================================
 */

/**
 * @brief Build an IPv4 frame and queue it on the PAL.
 *
 * @details Resolves the next-hop MAC via ARP (or falls back to
 *          broadcast on unresolved), writes the Ethernet + IPv4
 *          headers, and pushes through the PAL TX path. RFC 791.
 *
 * @param[in] dst         Destination IPv4 address.
 * @param[in] proto       IPv4 protocol number.
 * @param[in] payload     Upper-layer payload bytes (must not be NULL).
 * @param[in] payload_len Payload length.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                 Frame queued.
 * @retval k_ra_err_null_ptr       payload == NULL.
 * @retval k_ra_err_invalid_state  Stack not opened.
 * @retval k_ra_err_invalid_size   Combined frame exceeds PAL frame max.
 * @retval other                   ra_net_pal_send_frame error code.
 *
 * @pre ra_net_open has succeeded.
 * @pre payload is non-NULL.
 * @post On success a complete Ethernet+IPv4 frame is queued.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_ipv4_send(ra_net_ipv4_t     dst,
                          ra_net_ip_proto_t proto,
                          const uint8_t*    payload,
                          uint16_t          payload_len)
{
  if (payload == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (s_state.opened == 0U) {
    return k_ra_err_invalid_state;
  }
  uint16_t total = (uint16_t)(k_eth_hdr_len + k_ipv4_hdr_len + payload_len);
  if (total > (uint16_t)k_ra_net_pal_frame_max) {
    return k_ra_err_invalid_size;
  }

  static uint8_t s_tx[k_ra_net_pal_frame_max];

  /* Resolve next-hop MAC -- direct on subnet, else gateway. */
  ra_net_mac_t  dst_mac = {};
  ra_net_ipv4_t next    = dst;
  for (uint8_t i = 0U; i < 4U; i++) {
    if ((dst.bytes[i] & s_state.cfg.netmask.bytes[i]) !=
        (s_state.cfg.ip.bytes[i] & s_state.cfg.netmask.bytes[i])) {
      next = s_state.cfg.gateway;
      break;
    }
  }
  ra_err_t e = ra_net_arp_resolve(next, &dst_mac);
  if (e != k_ra_ok) {
    /* Unresolved -- fall back to broadcast so loopback tests still see the
     * packet; production code would queue and emit an ARP request. */
    for (uint8_t i = 0U; i < 6U; i++) {
      dst_mac.bytes[i] = 0xFFU;
    }
  }

  (void)memcpy(&s_tx[0], dst_mac.bytes, 6U);
  (void)memcpy(&s_tx[6], s_state.cfg.mac.bytes, 6U);
  ra_net_put_be16(&s_tx[12], (uint16_t)k_eth_type_ipv4);

  uint8_t* ip = &s_tx[k_eth_hdr_len];
  ip[0]       = (uint8_t)k_ipv4_version_ihl;
  ip[1]       = 0U;
  ra_net_put_be16(&ip[2], (uint16_t)(k_ipv4_hdr_len + payload_len));
  ra_net_put_be16(&ip[4], 0U); /* identification */
  ra_net_put_be16(&ip[6], 0U); /* flags + frag offset */
  ip[8] = (uint8_t)k_ipv4_default_ttl;
  ip[9] = (uint8_t)proto;
  ra_net_put_be16(&ip[10], 0U); /* checksum placeholder */
  (void)memcpy(&ip[12], s_state.cfg.ip.bytes, 4U);
  (void)memcpy(&ip[16], dst.bytes, 4U);
  uint16_t ck = ra_net_checksum_ones(ip, k_ipv4_hdr_len, 0U);
  ra_net_put_be16(&ip[10], ck);

  (void)memcpy(&s_tx[k_eth_hdr_len + k_ipv4_hdr_len], payload, payload_len);

  return ra_net_pal_send_frame(s_tx, total);
}

/* =============================================================================
 * Test hooks
 * =============================================================================
 */

/**
 * @brief Test hook -- inject a synthetic Ethernet frame into the RX path.
 *
 * @details Simulator-only path: copies the frame into an internal
 *          inject queue that ra_net_poll drains before touching the
 *          PAL. Lets unit tests stage a precise RX sequence without
 *          mocking the PAL.
 *
 * @param[in] frame Ethernet frame bytes.
 * @param[in] len   Frame length.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                 Frame queued.
 * @retval k_ra_err_null_ptr       frame == NULL.
 * @retval k_ra_err_invalid_state  Stack not opened.
 * @retval k_ra_err_invalid_arg    len out of range.
 * @retval k_ra_err_no_mem         Inject queue full.
 * @retval k_ra_err_not_supported  Not a simulator build.
 *
 * @pre ra_net_open has succeeded.
 * @pre Built with RA_SIMULATOR_MODE for the success path.
 * @post On success the inject queue gained one row.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe; for unit-test harness use only.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_test_inject_frame(const uint8_t* frame, uint16_t len)
{
  if (frame == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (s_state.opened == 0U) {
    return k_ra_err_invalid_state;
  }
  if ((len < k_eth_hdr_len) || (len > (uint16_t)k_ra_net_pal_frame_max)) {
    return k_ra_err_invalid_arg;
  }
#ifdef RA_SIMULATOR_MODE
  if (s_inject_count >= (uint16_t)k_inject_q_depth) {
    return k_ra_err_no_mem;
  }
  s_inject_q[s_inject_tail].len = len;
  (void)memcpy(s_inject_q[s_inject_tail].data, frame, len);
  s_inject_tail = (uint16_t)((s_inject_tail + 1U) % (uint16_t)k_inject_q_depth);
  s_inject_count++;
  return k_ra_ok;
#else
  return k_ra_err_not_supported;
#endif
}

/**
 * @brief Test hook -- drain one TX frame from the PAL ring.
 *
 * @details Lets unit tests inspect the bytes the stack actually
 *          emitted by reading them straight back out of the PAL TX
 *          ring (which the simulator uses as a snapshot buffer).
 *
 * @param[out] out_buf   Destination buffer.
 * @param[in,out] inout_len On input, capacity; on output, copied length.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                 Frame copied.
 * @retval k_ra_err_null_ptr       out_buf or inout_len is NULL.
 * @retval k_ra_err_invalid_state  Stack not opened.
 * @retval k_ra_err_no_data        TX ring empty.
 *
 * @pre ra_net_open has succeeded.
 * @pre Both pointers are non-NULL.
 * @post On success out_buf holds one TX frame and *inout_len is its length.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe; for unit-test harness use only.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_test_drain_tx(uint8_t* out_buf, uint16_t* inout_len)
{
  if ((out_buf == nullptr) || (inout_len == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if (s_state.opened == 0U) {
    return k_ra_err_invalid_state;
  }
  return ra_net_pal_recv_frame(out_buf, inout_len);
}

/* =============================================================================
 * Public socket facade -- thin shims that route to UDP / TCP TUs.
 * =============================================================================
 */

/**
 * @brief Public facade: allocate a UDP socket bound to local_port.
 *
 * @details Thin wrapper over ra_net_udp_socket that adds an
 *          opened-check.
 *
 * @param[in]  local_port Local port (must be non-zero).
 * @param[out] out_handle Filled with the socket handle on success.
 *
 * @return ra_err_t Error code (see ra_net_udp_socket).
 * @retval k_ra_ok                 Socket allocated.
 * @retval k_ra_err_invalid_state  Stack not opened.
 * @retval other                   ra_net_udp_socket error code.
 *
 * @pre ra_net_open has succeeded.
 * @pre out_handle is non-NULL.
 * @post On success a UDP socket slot is bound to local_port.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_socket_udp(uint16_t local_port, ra_net_handle_t* out_handle)
{
  if (s_state.opened == 0U) {
    return k_ra_err_invalid_state;
  }
  return ra_net_udp_socket(local_port, out_handle);
}

/**
 * @brief Public facade: allocate a passive TCP socket on local_port.
 *
 * @param[in]  local_port Local port (must be non-zero).
 * @param[out] out_handle Filled with the socket handle on success.
 *
 * @return ra_err_t Error code (see ra_net_tcp_listen).
 * @retval k_ra_ok                 Socket in LISTEN.
 * @retval k_ra_err_invalid_state  Stack not opened.
 * @retval other                   ra_net_tcp_listen error code.
 *
 * @pre ra_net_open has succeeded.
 * @pre out_handle is non-NULL.
 * @post On success a TCP socket is in LISTEN.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 */
ra_err_t ra_net_socket_tcp_listen(uint16_t local_port, ra_net_handle_t* out_handle)
{
  if (s_state.opened == 0U) {
    return k_ra_err_invalid_state;
  }
  return ra_net_tcp_listen(local_port, out_handle);
}

/**
 * @brief Public facade: initiate an active TCP open.
 *
 * @param[in]  remote_ip   Destination IPv4 address.
 * @param[in]  remote_port Destination TCP port.
 * @param[out] out_handle  Filled with the socket handle on success.
 *
 * @return ra_err_t Error code (see ra_net_tcp_connect).
 * @retval k_ra_ok                 SYN sent.
 * @retval k_ra_err_invalid_state  Stack not opened.
 * @retval other                   ra_net_tcp_connect error code.
 *
 * @pre ra_net_open has succeeded.
 * @pre out_handle is non-NULL.
 * @post On success a TCP socket is in SYN_SENT and a SYN has been queued.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 */
ra_err_t ra_net_socket_tcp_connect(ra_net_ipv4_t    remote_ip,
                                   uint16_t         remote_port,
                                   ra_net_handle_t* out_handle)
{
  if (s_state.opened == 0U) {
    return k_ra_err_invalid_state;
  }
  return ra_net_tcp_connect(remote_ip, remote_port, out_handle);
}

/**
 * @brief Public facade: send bytes on a UDP or TCP socket.
 *
 * @details Routes to ra_net_udp_send or ra_net_tcp_send based on the
 *          socket kind.
 *
 * @param[in] handle      Socket handle.
 * @param[in] buf         Payload bytes (must not be NULL).
 * @param[in] len         Payload length (> 0).
 * @param[in] remote_ip   Destination IPv4 (UDP only; ignored for TCP).
 * @param[in] remote_port Destination port (UDP only; ignored for TCP).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                 Bytes queued.
 * @retval k_ra_err_invalid_state  Stack not opened or wrong socket kind.
 * @retval k_ra_err_null_ptr       buf == NULL.
 * @retval k_ra_err_invalid_arg    len == 0 or handle out of range.
 * @retval other                   Whatever the per-protocol send returns.
 *
 * @pre ra_net_open has succeeded.
 * @pre handle was returned by a socket-creation API.
 * @post On success the per-protocol send has queued bytes.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_send(ra_net_handle_t handle,
                     const uint8_t*  buf,
                     uint16_t        len,
                     ra_net_ipv4_t   remote_ip,
                     uint16_t        remote_port)
{
  if (s_state.opened == 0U) {
    return k_ra_err_invalid_state;
  }
  if (buf == nullptr) {
    return k_ra_err_null_ptr;
  }
  if ((len == 0U) || (handle >= (uint8_t)k_ra_net_max_sockets)) {
    return k_ra_err_invalid_arg;
  }
  ra_net_socket_t* s = &s_state.socks[handle];
  if (s->kind == k_ra_net_sock_udp) {
    return ra_net_udp_send(handle, buf, len, remote_ip, remote_port);
  }
  if (s->kind == k_ra_net_sock_tcp) {
    return ra_net_tcp_send(handle, buf, len);
  }
  return k_ra_err_invalid_state;
}

/**
 * @brief Public facade: receive bytes from a UDP or TCP socket.
 *
 * @details Routes to ra_net_udp_recv or ra_net_tcp_recv based on the
 *          socket kind.
 *
 * @param[in]  handle      Socket handle.
 * @param[out] buf         Destination buffer.
 * @param[in]  max_len     Capacity of buf (> 0).
 * @param[out] got_len     Filled with the bytes copied.
 * @param[out] remote_ip   Filled with the source IPv4.
 * @param[out] remote_port Filled with the source port.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                 Bytes returned.
 * @retval k_ra_err_invalid_state  Stack not opened or wrong socket kind.
 * @retval k_ra_err_null_ptr       Required output pointer is NULL.
 * @retval k_ra_err_invalid_arg    max_len == 0 or handle out of range.
 * @retval k_ra_err_no_data        No data queued.
 *
 * @pre ra_net_open has succeeded.
 * @pre handle was returned by a socket-creation API.
 * @post On success buf holds *got_len bytes.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_recv(ra_net_handle_t handle,
                     uint8_t*        buf,
                     uint16_t        max_len,
                     uint16_t*       got_len,
                     ra_net_ipv4_t*  remote_ip,
                     uint16_t*       remote_port)
{
  if (s_state.opened == 0U) {
    return k_ra_err_invalid_state;
  }
  if ((buf == nullptr) || (got_len == nullptr) || (remote_ip == nullptr) ||
      (remote_port == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if ((max_len == 0U) || (handle >= (uint8_t)k_ra_net_max_sockets)) {
    return k_ra_err_invalid_arg;
  }
  ra_net_socket_t* s = &s_state.socks[handle];
  if (s->kind == k_ra_net_sock_udp) {
    return ra_net_udp_recv(handle, buf, max_len, got_len, remote_ip, remote_port);
  }
  if (s->kind == k_ra_net_sock_tcp) {
    return ra_net_tcp_recv(handle, buf, max_len, got_len, remote_ip, remote_port);
  }
  return k_ra_err_invalid_state;
}

/**
 * @brief Public facade: tear down a socket.
 *
 * @details For TCP sockets routes to ra_net_tcp_close (which performs
 *          the active close handshake); for UDP simply zeroes the
 *          slot.
 *
 * @param[in] handle Socket handle.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                 Socket closed (or close initiated).
 * @retval k_ra_err_invalid_state  Stack not opened.
 * @retval k_ra_err_invalid_arg    handle out of range or unused slot.
 * @retval other                   Whatever ra_net_tcp_close returns.
 *
 * @pre ra_net_open has succeeded.
 * @pre handle was returned by a socket-creation API.
 * @post On success the slot is closed or in active-close transition.
 * @post On failure no state mutation.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
ra_err_t ra_net_close_socket(ra_net_handle_t handle)
{
  if (s_state.opened == 0U) {
    return k_ra_err_invalid_state;
  }
  if (handle >= (uint8_t)k_ra_net_max_sockets) {
    return k_ra_err_invalid_arg;
  }
  ra_net_socket_t* s = &s_state.socks[handle];
  if (s->kind == k_ra_net_sock_tcp) {
    return ra_net_tcp_close(handle);
  }
  if (s->kind == k_ra_net_sock_udp) {
    (void)memset(s, 0, sizeof(*s));
    return k_ra_ok;
  }
  return k_ra_err_invalid_arg;
}

/* NOLINTEND(readability-magic-numbers,readability-redundant-casting,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,readability-function-size,readability-identifier-naming) */
