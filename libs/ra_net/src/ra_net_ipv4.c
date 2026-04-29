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

ra_net_state_t* ra_net_internal_state(void)
{
  return &s_state;
}

/* =============================================================================
 * Internet checksum (RFC 1071)
 * =============================================================================
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

ra_err_t ra_net_socket_udp(uint16_t local_port, ra_net_handle_t* out_handle)
{
  if (s_state.opened == 0U) {
    return k_ra_err_invalid_state;
  }
  return ra_net_udp_socket(local_port, out_handle);
}

ra_err_t ra_net_socket_tcp_listen(uint16_t local_port, ra_net_handle_t* out_handle)
{
  if (s_state.opened == 0U) {
    return k_ra_err_invalid_state;
  }
  return ra_net_tcp_listen(local_port, out_handle);
}

ra_err_t ra_net_socket_tcp_connect(ra_net_ipv4_t    remote_ip,
                                   uint16_t         remote_port,
                                   ra_net_handle_t* out_handle)
{
  if (s_state.opened == 0U) {
    return k_ra_err_invalid_state;
  }
  return ra_net_tcp_connect(remote_ip, remote_port, out_handle);
}

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
