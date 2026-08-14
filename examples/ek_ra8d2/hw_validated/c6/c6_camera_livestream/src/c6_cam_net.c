/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_camera_livestream/src/c6_cam_net.c
 * @brief NetX DHCP and a single-connection HTTP camera server over ESP32-C6.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details The C6 remains the validated esp-hosted L2 co-processor. NetX Duo,
 * DHCP, TCP, HTTP parsing, camera capture and JPEG generation all run on RA8.
 * The page refreshes `/frame.jpg` after each completed load, producing a live
 * stream without requiring multipart/MJPEG middleware or FileX.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "c6_camera_livestream.h"
#include "nx_api.h"
#include "nx_ether_driver_c6.h"
#include "nxd_dhcp_client.h"
#include "ra8_c6link.h"
#include "ra8_err.h"
#include "tx_api.h"

static NX_PACKET_POOL s_pool;
alignas(4) static uint8_t s_pool_mem[k_c6_cam_net_pool_bytes];
static NX_IP s_ip;
alignas(8) static uint8_t s_ip_stack[k_c6_cam_net_ip_stack];
alignas(4) static uint8_t s_arp_cache[k_c6_cam_net_arp_bytes];
static NX_DHCP       s_dhcp;
static NX_TCP_SOCKET s_http_socket;
static CHAR          s_pool_name[]   = "c6_cam_pool";
static CHAR          s_ip_name[]     = "c6_cam_ip";
static CHAR          s_dhcp_name[]   = "c6_cam_dhcp";
static CHAR          s_socket_name[] = "c6_cam_http";

static const char s_page[] =
  "<!doctype html><html><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>EK-RA8D2 Camera</title><style>body{margin:0;background:#111;color:#eee;"
  "font:16px system-ui;display:grid;place-items:center;min-height:100vh}main{text-align:center}"
  "img{width:min(96vw,960px);height:auto;image-rendering:auto;border:1px solid #555}"
  "small{display:block;margin-top:.5rem;color:#aaa}</style></head><body><main>"
  "<h1>EK-RA8D2 / OV5640</h1><img id=c alt='live camera'>"
  "<small id=s>connecting</small><script>const i=document.querySelector('#c'),"
  "s=document.querySelector('#s');let n=0,t=performance.now();function next(){"
  "i.src='/frame.jpg?seq='+(++n)+'&t='+Date.now()}i.onload=()=>{let q=performance.now();"
  "s.textContent='live - '+(1000/(q-t)).toFixed(1)+' fps';t=q;setTimeout(next,25)};"
  "i.onerror=()=>{s.textContent='retrying';setTimeout(next,1000)};next()</script>"
  "</main></body></html>";

static UINT c6_cam_net_create(void)
{
  UINT status = nx_packet_pool_create(&s_pool,
                                      s_pool_name,
                                      (ULONG)k_c6_cam_net_pkt_payload,
                                      (VOID*)s_pool_mem,
                                      (ULONG)sizeof(s_pool_mem));
  if (status != NX_SUCCESS) {
    return status;
  }
  status = nx_ip_create(&s_ip,
                        s_ip_name,
                        IP_ADDRESS(0, 0, 0, 0),
                        IP_ADDRESS(0, 0, 0, 0),
                        &s_pool,
                        nx_ether_driver_c6,
                        (VOID*)s_ip_stack,
                        (ULONG)sizeof(s_ip_stack),
                        (UINT)k_c6_cam_net_ip_prio);
  if (status != NX_SUCCESS) {
    return status;
  }
  status = nx_arp_enable(&s_ip, (VOID*)s_arp_cache, (ULONG)sizeof(s_arp_cache));
  if (status != NX_SUCCESS) {
    return status;
  }
  status = nx_udp_enable(&s_ip);
  if (status != NX_SUCCESS) {
    return status;
  }
  status = nx_tcp_enable(&s_ip);
  if (status != NX_SUCCESS) {
    return status;
  }
  return nx_icmp_enable(&s_ip);
}

ra8_err_t c6_cam_net_up(ra8_c6link_t* link, const ra8_c6link_mac_t* mac, c6_cam_lease_t* out)
{
  if ((link == nullptr) || (mac == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out = (c6_cam_lease_t){};
  nx_ether_driver_c6_bind(link);
  nx_ether_driver_c6_set_mac(mac->octet);
  nx_system_initialize();
  if (c6_cam_net_create() != NX_SUCCESS) {
    return k_ra8_err_not_initialized;
  }
  UINT status = nx_dhcp_create(&s_dhcp, &s_ip, s_dhcp_name);
  if (status == NX_SUCCESS) {
    status = nx_dhcp_start(&s_dhcp);
  }
  ULONG actual = 0U;
  if (status == NX_SUCCESS) {
    status = nx_ip_status_check(&s_ip,
                                (ULONG)NX_IP_ADDRESS_RESOLVED,
                                &actual,
                                (ULONG)k_c6_cam_dhcp_wait_ms);
  }
  if (status != NX_SUCCESS) {
    return k_ra8_err_timeout;
  }
  ULONG ip      = 0U;
  ULONG mask    = 0U;
  ULONG gateway = 0U;
  ULONG server  = 0U;
  (void)nx_ip_address_get(&s_ip, &ip, &mask);
  (void)nx_ip_gateway_address_get(&s_ip, &gateway);
  (void)nx_dhcp_server_address_get(&s_dhcp, &server);
  out->ip          = (uint32_t)ip;
  out->mask        = (uint32_t)mask;
  out->gateway     = (uint32_t)gateway;
  out->dhcp_server = (uint32_t)server;
  out->bound       = (ip != 0U);
  return out->bound ? k_ra8_ok : k_ra8_err_timeout;
}

static UINT c6_cam_http_send(const void* data, uint32_t bytes)
{
  const uint8_t* cursor    = (const uint8_t*)data;
  uint32_t       remaining = bytes;
  while (remaining != 0U) {
    const uint32_t chunk =
      (remaining > (uint32_t)k_c6_cam_http_chunk) ? (uint32_t)k_c6_cam_http_chunk : remaining;
    NX_PACKET* packet = NX_NULL;
    UINT       status = nx_packet_allocate(&s_pool, &packet, NX_TCP_PACKET, NX_WAIT_FOREVER);
    if (status != NX_SUCCESS) {
      return status;
    }
    status = nx_packet_data_append(packet,
                                   (VOID*)(uintptr_t)cursor,
                                   (ULONG)chunk,
                                   &s_pool,
                                   NX_WAIT_FOREVER);
    if (status != NX_SUCCESS) {
      (void)nx_packet_release(packet);
      return status;
    }
    status = nx_tcp_socket_send(&s_http_socket, packet, NX_WAIT_FOREVER);
    if (status != NX_SUCCESS) {
      (void)nx_packet_release(packet);
      return status;
    }
    cursor += chunk;
    remaining -= chunk;
  }
  return NX_SUCCESS;
}

static uint32_t c6_cam_append(char* out, uint32_t at, uint32_t cap, const char* text)
{
  while ((at < cap) && (*text != '\0')) {
    out[at++] = *text++;
  }
  return at;
}

/** @brief Decimal formatting and minimum HTTP request sizes. */
typedef enum : uint32_t {
  k_c6_cam_net_decimal_digits = 10U, /**< Digits in a 32-bit decimal rendering. */
  k_c6_cam_http_get_min       = 5U,  /**< Minimum bytes in an HTTP GET request. */
} c6_cam_net_format_t;

static uint32_t c6_cam_append_u32(char* out, uint32_t at, uint32_t cap, uint32_t value)
{
  char     reverse[k_c6_cam_net_decimal_digits] = {};
  uint32_t count                                = 0U;
  do {
    reverse[count++] = (char)('0' + (value % (uint32_t)k_c6_cam_net_decimal_digits));
    value /= (uint32_t)k_c6_cam_net_decimal_digits;
  } while ((value != 0U) && (count < (uint32_t)k_c6_cam_net_decimal_digits));
  while ((count != 0U) && (at < cap)) {
    out[at++] = reverse[--count];
  }
  return at;
}

static UINT c6_cam_http_response(const char* type, const void* body, uint32_t body_bytes)
{
  char     header[192] = {};
  uint32_t length = c6_cam_append(header, 0U, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: ");
  length          = c6_cam_append(header, length, sizeof(header), type);
  length          = c6_cam_append(header, length, sizeof(header), "\r\nContent-Length: ");
  length          = c6_cam_append_u32(header, length, sizeof(header), body_bytes);
  length      = c6_cam_append(header,
                              length,
                              sizeof(header),
                              "\r\nCache-Control: no-store, no-cache\r\nConnection: close\r\n\r\n");
  UINT status = c6_cam_http_send(header, length);
  if (status == NX_SUCCESS) {
    status = c6_cam_http_send(body, body_bytes);
  }
  return status;
}

static bool c6_cam_request_is(const char* request, uint32_t bytes, const char* path)
{
  if ((bytes < (uint32_t)k_c6_cam_http_get_min) || (memcmp(request, "GET ", 4U) != 0)) {
    return false;
  }
  uint32_t index      = 4U;
  uint32_t path_index = 0U;
  while ((index < bytes) && (path[path_index] != '\0') && (request[index] == path[path_index])) {
    index++;
    path_index++;
  }
  if (path[path_index] != '\0') {
    return false;
  }
  return (index < bytes) && ((request[index] == ' ') || (request[index] == '?'));
}

static void c6_cam_http_handle(void)
{
  NX_PACKET* request_packet = NX_NULL;
  if (nx_tcp_socket_receive(&s_http_socket, &request_packet, NX_WAIT_FOREVER) != NX_SUCCESS) {
    return;
  }
  ULONG packet_bytes = 0U;
  (void)nx_packet_length_get(request_packet, &packet_bytes);
  uint32_t request_bytes                 = (packet_bytes > (ULONG)k_c6_cam_request_max)
                                             ? (uint32_t)k_c6_cam_request_max
                                             : (uint32_t)packet_bytes;
  char     request[k_c6_cam_request_max] = {};
  ULONG    copied                        = 0U;
  (void)nx_packet_data_extract_offset(request_packet, 0U, request, (ULONG)request_bytes, &copied);
  (void)nx_packet_release(request_packet);

  if (c6_cam_request_is(request, (uint32_t)copied, "/frame.jpg")) {
    const uint8_t*  jpeg       = nullptr;
    uint32_t        jpeg_bytes = 0U;
    const ra8_err_t err        = c6_cam_camera_capture_jpeg(&jpeg, &jpeg_bytes);
    if (err == k_ra8_ok) {
      (void)c6_cam_http_response("image/jpeg", jpeg, jpeg_bytes);
      c6_cam_puts("c6_cam: frame jpeg_bytes=");
      c6_cam_put_u32(jpeg_bytes);
      c6_cam_puts("\r\n");
    } else {
      static const char failed[] = "camera capture failed\n";
      (void)c6_cam_http_response("text/plain", failed, (uint32_t)(sizeof(failed) - 1U));
      c6_cam_puts("c6_cam: frame FAIL err=");
      c6_cam_puts(ra8_err_to_str(err));
      c6_cam_puts("\r\n");
    }
  } else if (c6_cam_request_is(request, (uint32_t)copied, "/health")) {
    static const char healthy[] = "PASS c6 camera livestream\n";
    (void)c6_cam_http_response("text/plain", healthy, (uint32_t)(sizeof(healthy) - 1U));
  } else {
    (void)c6_cam_http_response("text/html; charset=utf-8", s_page, (uint32_t)(sizeof(s_page) - 1U));
  }
}

void c6_cam_http_serve(void)
{
  UINT status = nx_tcp_socket_create(&s_ip,
                                     &s_http_socket,
                                     s_socket_name,
                                     NX_IP_NORMAL,
                                     NX_FRAGMENT_OKAY,
                                     NX_IP_TIME_TO_LIVE,
                                     4096U,
                                     NX_NULL,
                                     NX_NULL);
  if (status == NX_SUCCESS) {
    status =
      nx_tcp_server_socket_listen(&s_ip, (UINT)k_c6_cam_http_port, &s_http_socket, 1U, NX_NULL);
  }
  if (status != NX_SUCCESS) {
    c6_cam_puts("c6_cam: FAIL HTTP socket setup\r\n");
    while (true) {
      tx_thread_sleep(NX_IP_PERIODIC_RATE);
    }
  }
  c6_cam_puts("c6_cam: PASS HTTP camera server listening\r\n");
  while (true) {
    if (nx_tcp_server_socket_accept(&s_http_socket, NX_WAIT_FOREVER) == NX_SUCCESS) {
      c6_cam_http_handle();
      (void)nx_tcp_socket_disconnect(&s_http_socket, NX_WAIT_FOREVER);
      (void)nx_tcp_server_socket_unaccept(&s_http_socket);
    }
    (void)nx_tcp_server_socket_relisten(&s_ip, (UINT)k_c6_cam_http_port, &s_http_socket);
  }
}
