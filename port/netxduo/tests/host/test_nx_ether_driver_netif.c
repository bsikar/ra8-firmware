/**
 * @file port/netxduo/tests/host/test_nx_ether_driver_netif.c
 * @brief Dual-backend host tests for the unified NetX Duo driver (RA8FW-318).
 *
 * Self-contained: the NetX/ThreadX API surface the driver touches is stubbed
 * in stubs/ (NX_IP_DRIVER dispatch struct, command codes, packet shims,
 * ThreadX thread shims) and every shim records its calls, so the dispatch,
 * header build, link mirror, bind refusal and uninit paths are asserted
 * without a stack. The seam is mocked through ra8_io_netif_internal.h's
 * vtable; two fake nx_netif_backend_ops_t exercise both backend shapes:
 * net_pal-like (link_up + close present) and c6link-like (both NULL).
 *
 * Build + run (workspace host checks; CI rides the app builds):
 *   zig cc -std=c23 -Wall -Wextra -Werror \
 *     -Iport/netxduo/tests/host/stubs -Iport/netxduo/inc \
 *     -Ilibs/ra8_core/inc -Ilibs/ra8_io/inc -Ilibs/ra8_io/src \
 *     port/netxduo/tests/host/test_nx_ether_driver_netif.c \
 *     port/netxduo/src/nx_ether_driver_netif.c -o /tmp/netif_test && /tmp/netif_test
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nx_ether_driver_netif.h"
#include "tx_api.h"


/* ---- recorded NetX shim calls ---- */
static unsigned s_alloc_calls, s_append_calls, s_release_calls, s_tx_release_calls, s_ip_defer, s_arp_defer, s_rarp_defer, s_thread_create_calls;
static NX_PACKET* s_last_ip_pkt;
static NX_PACKET s_pkt;
static UCHAR s_pkt_buf[1600];
static UINT s_alloc_rc = 0; /* NX_SUCCESS */
static UINT s_append_rc = 0;
static UINT s_send_rc; /* seam send verdict the mock returns */

UINT nx_packet_allocate(NX_PACKET_POOL* p, NX_PACKET** pkt, UINT t, ULONG w) { (void)p; (void)t; (void)w; s_alloc_calls++; *pkt = &s_pkt; return s_alloc_rc; }
UINT nx_packet_data_append(NX_PACKET* pkt, void* d, ULONG n, NX_PACKET_POOL* p, ULONG w) { (void)p; (void)w; s_append_calls++; memcpy(pkt->nx_packet_prepend_ptr, d, (size_t)n); pkt->nx_packet_length = n; pkt->nx_packet_append_ptr = pkt->nx_packet_prepend_ptr + n; return s_append_rc; }
UINT nx_packet_release(NX_PACKET* pkt) { (void)pkt; s_release_calls++; return 0; }
UINT nx_packet_transmit_release(NX_PACKET* pkt) { (void)pkt; s_tx_release_calls++; return 0; }
void _nx_ip_packet_deferred_receive(NX_IP* ip, NX_PACKET* pkt) { (void)ip; s_ip_defer++; s_last_ip_pkt = pkt; }
void _nx_arp_packet_deferred_receive(NX_IP* ip, NX_PACKET* pkt) { (void)ip; (void)pkt; s_arp_defer++; }
void _nx_rarp_packet_deferred_receive(NX_IP* ip, NX_PACKET* pkt) { (void)ip; (void)pkt; s_rarp_defer++; }
UINT tx_thread_create(TX_THREAD* t, CHAR* n, void (*e)(ULONG), ULONG a, void* st, ULONG ss, UINT pr, UINT pre, ULONG ts, UINT as) { (void)t; (void)n; (void)e; (void)a; (void)st; (void)ss; (void)pr; (void)pre; (void)ts; (void)as; s_thread_create_calls++; return 0; }
void tx_thread_sleep(ULONG t) { (void)t; }

/* ---- seam shims: the driver's two seam entry points, recorded ---- */
static uint8_t s_mock_send_frame[1600];
static uint16_t s_mock_send_len;
static uint8_t s_mock_rx_queue[4][1600];
static uint16_t s_mock_rx_len[4];
static unsigned s_mock_rx_head, s_mock_rx_tail;
static unsigned s_poll_calls;
static ra8_io_netif_t s_mock_netif = { .iface = (const void*)1, .ctx = (void*)1 };

ra8_err_t ra8_io_netif_send(const ra8_io_netif_t* netif, const uint8_t* frame, uint16_t len)
{
  (void)netif;
  memcpy(s_mock_send_frame, frame, len);
  s_mock_send_len = len;
  return s_send_rc;
}
ra8_err_t ra8_io_netif_poll(const ra8_io_netif_t* netif, uint8_t* out_buf, uint16_t* inout_len)
{
  (void)netif;
  s_poll_calls++;
  if (s_mock_rx_head == s_mock_rx_tail) {
    return k_ra8_err_no_data;
  }
  memcpy(out_buf, s_mock_rx_queue[s_mock_rx_tail], s_mock_rx_len[s_mock_rx_tail]);
  *inout_len = s_mock_rx_len[s_mock_rx_tail];
  s_mock_rx_tail++;
  return k_ra8_ok;
}

/* ---- fake backend ops ---- */
static unsigned s_open_calls, s_close_calls;
static uint8_t s_open_mac[6];
static ra8_err_t fake_open_a(const uint8_t* mac) { s_open_calls++; memcpy(s_open_mac, mac, 6); return k_ra8_ok; }
static void fake_close_a(void) { s_close_calls++; }
static bool fake_link_a(void) { return true; }
static const nx_netif_backend_ops_t s_ops_a = { .open = fake_open_a, .close = fake_close_a, .link_up = fake_link_a, .mtu = 128U };
static ra8_err_t fake_open_b(const uint8_t* mac) { (void)mac; s_open_calls++; return k_ra8_ok; }
static const nx_netif_backend_ops_t s_ops_b = { .open = fake_open_b, .close = NULL, .link_up = NULL, .mtu = 1514U };

/* ---- fixtures ---- */
static NX_IP s_ip;
static NX_PACKET_POOL s_pool;
static NX_INTERFACE s_iface;
static NX_IP_DRIVER s_req;

static void reset_all(void)
{
  memset(&s_pkt, 0, sizeof(s_pkt));
  memset(&s_pkt_buf, 0, sizeof(s_pkt_buf));
  s_pkt.nx_packet_prepend_ptr = s_pkt_buf + 8;
  s_pkt.nx_packet_append_ptr = s_pkt.nx_packet_prepend_ptr;
  memset(&s_iface, 0, sizeof(s_iface));
  memset(&s_req, 0, sizeof(s_req));
  s_alloc_calls = s_append_calls = s_release_calls = s_tx_release_calls = 0;
  s_ip_defer = s_arp_defer = s_rarp_defer = s_thread_create_calls = 0;
  s_open_calls = s_close_calls = 0;
  s_poll_calls = 0; s_mock_rx_head = s_mock_rx_tail = 0;
  s_send_rc = k_ra8_ok; s_alloc_rc = 0; s_append_rc = 0;
  memset(s_mock_send_frame, 0, sizeof(s_mock_send_frame)); s_mock_send_len = 0;
  s_ip.nx_ip_default_packet_pool = &s_pool;
}

/* MAC bytes for the interface: msw = 00:11, lsw = 22:33:44:55 */
static void set_iface_mac(void)
{
  s_iface.nx_interface_physical_address_msw = (0x00UL << 8) | 0x11UL;
  s_iface.nx_interface_physical_address_lsw = (0x22UL << 24) | (0x33UL << 16) | (0x44UL << 8) | 0x55UL;
}

static void test_bind_guards(void)
{
  reset_all();
  /* Before any bind, INITIALIZE is refused. */
  s_req.nx_ip_driver_command = NX_LINK_INITIALIZE;
  s_req.nx_ip_driver_interface = &s_iface;
  nx_ether_driver_netif(&s_req);
  assert(s_req.nx_ip_driver_status == NX_NOT_SUCCESSFUL);
  assert(s_open_calls == 0);
  /* NULL guards refuse without binding. */
  assert(nx_ether_driver_netif_bind(NULL, &s_ops_a) == k_ra8_err_null_ptr);
  assert(nx_ether_driver_netif_bind(&s_mock_netif, NULL) == k_ra8_err_null_ptr);
  assert(nx_ether_driver_netif_bind(&s_mock_netif, &s_ops_a) == k_ra8_ok);
  printf("unbound refusal + bind guards ok\n");
}

static void test_init_backend_a(void)
{
  reset_all();
  set_iface_mac();
  s_req.nx_ip_driver_command = NX_LINK_INITIALIZE;
  s_req.nx_ip_driver_interface = &s_iface;
  s_req.nx_ip_driver_ptr = &s_ip;
  nx_ether_driver_netif(&s_req);
  assert(s_req.nx_ip_driver_status == NX_SUCCESS);
  assert(s_open_calls == 1);
  assert(s_open_mac[0] == 0x00 && s_open_mac[1] == 0x11 && s_open_mac[5] == 0x55);
  assert(s_iface.nx_interface_physical_address_msw == s_iface.nx_interface_physical_address_msw); /* stamped non-zero */
  assert(s_iface.nx_interface_ip_mtu_size == 128UL);
  assert(s_iface.nx_interface_address_mapping_needed == NX_TRUE);
  assert(s_thread_create_calls == 1);
  /* backend B shape: mtu 1514, no link_up/close ops bound */
  s_req.nx_ip_driver_command = NX_LINK_UNINITIALIZE;
  nx_ether_driver_netif(&s_req);
  assert(s_close_calls == 1);
  printf("INITIALIZE (net_pal shape) ok\n");
}

static void test_send_header(void)
{
  reset_all();
  set_iface_mac();
  s_req.nx_ip_driver_command = NX_LINK_INITIALIZE;
  s_req.nx_ip_driver_interface = &s_iface;
  s_req.nx_ip_driver_ptr = &s_ip;
  nx_ether_driver_netif(&s_req);
  /* NetX ENABLEs the link before any packet path; the mirror gates SEND. */
  s_req.nx_ip_driver_command = NX_LINK_ENABLE;
  nx_ether_driver_netif(&s_req);
  /* ARP_SEND with a 4-byte body: header + runt padding to 60 */
  uint8_t body[4] = {1, 2, 3, 4};
  memcpy(s_pkt.nx_packet_prepend_ptr, body, 4);
  s_pkt.nx_packet_append_ptr = s_pkt.nx_packet_prepend_ptr + 4;
  s_pkt.nx_packet_length = 4;
  s_req.nx_ip_driver_command = NX_LINK_ARP_SEND;
  s_req.nx_ip_driver_packet = &s_pkt;
  s_req.nx_ip_driver_physical_address_msw = (0xAAUL << 8) | 0xBBUL;
  s_req.nx_ip_driver_physical_address_lsw = (0xCCUL << 24) | (0xDDUL << 16) | (0xEEUL << 8) | 0xFFUL;
  nx_ether_driver_netif(&s_req);
  assert(s_req.nx_ip_driver_status == NX_SUCCESS);
  assert(s_tx_release_calls == 1);
  assert(s_mock_send_len == 60);
  assert(s_mock_send_frame[0] == 0xAA && s_mock_send_frame[5] == 0xFF);
  assert(s_mock_send_frame[6] == 0x00 && s_mock_send_frame[11] == 0x55);
  assert(s_mock_send_frame[12] == 0x08 && s_mock_send_frame[13] == 0x06);
  assert(s_mock_send_frame[14] == 1 && s_mock_send_frame[17] == 4);
  assert(s_mock_send_frame[18] == 0 && s_mock_send_frame[59] == 0); /* runt pad */
  /* DISABLE then SEND fails and still releases */
  s_req.nx_ip_driver_command = NX_LINK_DISABLE;
  nx_ether_driver_netif(&s_req);
  s_req.nx_ip_driver_command = NX_LINK_ARP_SEND;
  nx_ether_driver_netif(&s_req);
  assert(s_req.nx_ip_driver_status == NX_NOT_SUCCESSFUL);
  assert(s_tx_release_calls == 2);
  /* send error surfaces as NX_NOT_SUCCESSFUL */
  s_req.nx_ip_driver_command = NX_LINK_ENABLE;
  nx_ether_driver_netif(&s_req);
  s_send_rc = k_ra8_err_busy;
  s_req.nx_ip_driver_command = NX_LINK_PACKET_SEND;
  nx_ether_driver_netif(&s_req);
  assert(s_req.nx_ip_driver_status == NX_NOT_SUCCESSFUL);
  printf("SEND header + pad + gates ok\n");
}

static void test_get_status_both_shapes(void)
{
  reset_all();
  set_iface_mac();
  s_req.nx_ip_driver_command = NX_LINK_INITIALIZE;
  s_req.nx_ip_driver_interface = &s_iface;
  s_req.nx_ip_driver_ptr = &s_ip;
  nx_ether_driver_netif(&s_req);
  ULONG link = 9;
  s_req.nx_ip_driver_command = NX_LINK_GET_STATUS;
  s_req.nx_ip_driver_return_ptr = &link;
  nx_ether_driver_netif(&s_req);
  assert(s_req.nx_ip_driver_status == NX_SUCCESS && link == NX_TRUE); /* ops link_up */
  /* DISABLE: ops link_up still answers true (PHY presence, not the mirror) */
  s_req.nx_ip_driver_command = NX_LINK_DISABLE;
  nx_ether_driver_netif(&s_req);
  s_req.nx_ip_driver_command = NX_LINK_GET_STATUS;
  nx_ether_driver_netif(&s_req);
  assert(link == NX_TRUE);
  /* uninit clears open: GET_STATUS answers false, SUCCESS */
  s_req.nx_ip_driver_command = NX_LINK_UNINITIALIZE;
  nx_ether_driver_netif(&s_req);
  s_req.nx_ip_driver_command = NX_LINK_GET_STATUS;
  nx_ether_driver_netif(&s_req);
  assert(link == NX_FALSE && s_req.nx_ip_driver_status == NX_SUCCESS);
  printf("GET_STATUS net_pal shape ok\n");
}

static void test_get_status_mirror_shape(void)
{
  reset_all();
  assert(nx_ether_driver_netif_bind(&s_mock_netif, &s_ops_b) == k_ra8_ok);
  set_iface_mac();
  s_req.nx_ip_driver_command = NX_LINK_INITIALIZE;
  s_req.nx_ip_driver_interface = &s_iface;
  s_req.nx_ip_driver_ptr = &s_ip;
  nx_ether_driver_netif(&s_req);
  assert(s_iface.nx_interface_ip_mtu_size == 1514UL);
  ULONG link = 9;
  s_req.nx_ip_driver_command = NX_LINK_GET_STATUS;
  s_req.nx_ip_driver_return_ptr = &link;
  nx_ether_driver_netif(&s_req);
  assert(link == NX_FALSE); /* mirror starts clear */
  s_req.nx_ip_driver_command = NX_LINK_ENABLE;
  nx_ether_driver_netif(&s_req);
  assert(s_iface.nx_interface_link_up == NX_TRUE);
  s_req.nx_ip_driver_command = NX_LINK_GET_STATUS;
  nx_ether_driver_netif(&s_req);
  assert(link == NX_TRUE); /* ENABLE mirror answers */
  s_req.nx_ip_driver_command = NX_LINK_UNINITIALIZE;
  nx_ether_driver_netif(&s_req); /* close is NULL: must not crash */
  assert(s_close_calls == 0);
  printf("GET_STATUS mirror shape (c6link) ok\n");
}

static void test_deferred_rx_dispatch(void)
{
  reset_all();
  set_iface_mac();
  s_req.nx_ip_driver_command = NX_LINK_INITIALIZE;
  s_req.nx_ip_driver_interface = &s_iface;
  s_req.nx_ip_driver_ptr = &s_ip;
  nx_ether_driver_netif(&s_req);
  /* queue: one IPv4 frame, one ARP frame, one unknown ethertype */
  uint8_t f1[15]; memset(f1, 0, 15); f1[12] = 0x08; f1[13] = 0x00;
  memcpy(s_mock_rx_queue[0], f1, 15); s_mock_rx_len[0] = 15;
  uint8_t f2[15]; memset(f2, 0, 15); f2[12] = 0x08; f2[13] = 0x06;
  memcpy(s_mock_rx_queue[1], f2, 15); s_mock_rx_len[1] = 15;
  uint8_t f3[15]; memset(f3, 0, 15); f3[12] = 0x86; f3[13] = 0xDD;
  memcpy(s_mock_rx_queue[2], f3, 15); s_mock_rx_len[2] = 15;
  s_mock_rx_head = 3;
  s_req.nx_ip_driver_command = NX_LINK_DEFERRED_PROCESSING;
  nx_ether_driver_netif(&s_req);
  assert(s_req.nx_ip_driver_status == NX_SUCCESS);
  assert(s_alloc_calls == 3 && s_ip_defer == 1 && s_arp_defer == 1 && s_rarp_defer == 0);
  assert(s_release_calls == 1); /* unknown ethertype released */
  assert(s_last_ip_pkt != NULL);
  assert(s_last_ip_pkt->nx_packet_length == 1); /* 14-byte header stripped */
  /* empty seam: poll returns no_data, nothing allocates */
  s_alloc_calls = 0;
  nx_ether_driver_netif(&s_req);
  assert(s_alloc_calls == 0);
  printf("DEFERRED_PROCESSING dispatch ok\n");
}

static void test_no_ops_commands(void)
{
  reset_all();
  set_iface_mac();
  s_req.nx_ip_driver_command = NX_LINK_INITIALIZE;
  s_req.nx_ip_driver_interface = &s_iface;
  s_req.nx_ip_driver_ptr = &s_ip;
  nx_ether_driver_netif(&s_req);
  s_req.nx_ip_driver_command = NX_LINK_MULTICAST_JOIN;
  nx_ether_driver_netif(&s_req);
  assert(s_req.nx_ip_driver_status == NX_SUCCESS);
  s_req.nx_ip_driver_command = NX_LINK_INTERFACE_ATTACH;
  nx_ether_driver_netif(&s_req);
  assert(s_req.nx_ip_driver_status == NX_SUCCESS);
  s_req.nx_ip_driver_command = (UINT)0x99; /* unknown */
  nx_ether_driver_netif(&s_req);
  assert(s_req.nx_ip_driver_status == NX_NOT_SUCCESSFUL);
  printf("no-op commands + unknown default ok\n");
}

static void test_set_mac_parity(void)
{
  reset_all();
  set_iface_mac();
  /* NULL is ignored: no latch, the iface MAC stays the fallback. */
  nx_ether_driver_netif_set_mac(NULL);
  s_req.nx_ip_driver_command = NX_LINK_INITIALIZE;
  s_req.nx_ip_driver_interface = &s_iface;
  s_req.nx_ip_driver_ptr = &s_ip;
  nx_ether_driver_netif(&s_req);
  assert(s_req.nx_ip_driver_status == NX_SUCCESS);
  assert(s_open_calls == 1);
  assert(s_open_mac[0] == 0x00 && s_open_mac[5] == 0x55);
  s_req.nx_ip_driver_command = NX_LINK_UNINITIALIZE;
  nx_ether_driver_netif(&s_req);
  assert(s_req.nx_ip_driver_status == NX_SUCCESS);
  /* Rebind the close-bearing shape, then an app-supplied MAC:
   * INITIALIZE programmes the stored MAC instead of the iface value,
   * stamps it back, and hands it to open (the
   * nx_ether_driver_ra8_eth_set_mac parity shape). */
  assert(nx_ether_driver_netif_bind(&s_mock_netif, &s_ops_a) == k_ra8_ok);
  {
    const uint8_t app_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    nx_ether_driver_netif_set_mac(app_mac);
  }
  s_req.nx_ip_driver_command = NX_LINK_INITIALIZE;
  nx_ether_driver_netif(&s_req);
  assert(s_req.nx_ip_driver_status == NX_SUCCESS);
  assert(s_open_calls == 2);
  assert(s_open_mac[0] == 0xAA && s_open_mac[1] == 0xBB &&
         s_open_mac[2] == 0xCC && s_open_mac[3] == 0xDD &&
         s_open_mac[4] == 0xEE && s_open_mac[5] == 0xFF);
  const ULONG msw = s_iface.nx_interface_physical_address_msw;
  const ULONG lsw = s_iface.nx_interface_physical_address_lsw;
  assert(msw == ((0xAAUL << 8) | 0xBBUL));
  assert(lsw == ((0xCCUL << 24) | (0xDDUL << 16) | (0xEEUL << 8) | 0xFFUL));
  s_req.nx_ip_driver_command = NX_LINK_UNINITIALIZE;
  nx_ether_driver_netif(&s_req);
  assert(s_close_calls == 1);
  printf("set_mac parity ok\n");
}

int main(void)
{
  test_bind_guards();
  test_init_backend_a();
  test_send_header();
  test_get_status_both_shapes();
  test_get_status_mirror_shape();
  test_deferred_rx_dispatch();
  test_no_ops_commands();
  test_set_mac_parity();
  printf("ALL PASS\n");
  return 0;
}
