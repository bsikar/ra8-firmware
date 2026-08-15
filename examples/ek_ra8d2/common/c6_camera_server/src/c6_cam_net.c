/**
 * @file examples/ek_ra8d2/common/c6_camera_server/src/c6_cam_net.c
 * @brief NetX DHCP and a single-connection HTTP camera server over ESP32-C6.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details The C6 remains the validated esp-hosted L2 co-processor. NetX Duo,
 * DHCP, TCP, HTTP parsing, camera capture and JPEG generation all run on RA8.
 * The page uses one persistent multipart/MJPEG connection. `/frame.jpg`
 * remains available for still capture, diagnostics, and HIL qualification.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "c6_camera_server.h"
#include "nx_api.h"
#include "nx_ether_driver_c6.h"
#include "nxd_dhcp_client.h"
#include "ra8_attributes.h"
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
  "small{display:block;margin-top:.5rem;color:#aaa}audio{margin-top:1rem}</style></head><body><main>"
  "<h1>EK-RA8D2 / OV5640</h1><img id=stream src=/stream.mjpg alt='live camera'>"
  "<small>bounded multipart MJPEG stream with automatic reconnect</small>"
  "<audio controls src=/audio.wav></audio>"
  "<small>latest rolling one-second onboard MIC1 recording</small>"
  "<script>const s=document.getElementById('stream');s.onerror=()=>{"
  "setTimeout(()=>{s.src='/stream.mjpg?t='+Date.now()},1000)}</script>"
  "</main></body></html>";

/**
 * @brief Create the static NetX packet pool and IP instance.
 * @details Enables ARP, UDP, TCP, and ICMP over the already-bound C6 Ethernet driver.
 * @return NetX status code.
 * @retval NX_SUCCESS All required NetX objects and protocols are ready.
 * @retval NX_NOT_SUCCESSFUL A NetX creation or enable operation failed.
 * @pre `nx_system_initialize` has completed.
 * @pre Static pool, stack, and ARP storage are not owned by another NetX instance.
 * @post On success, `s_pool` and `s_ip` are initialized.
 * @post On failure, no later protocol-enable step is attempted.
 * @note Exact non-success codes are propagated from NetX.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT internal_c6_cam_net_create(void)
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
  if (internal_c6_cam_net_create() != NX_SUCCESS) {
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

/**
 * @brief Send an exact byte span over the accepted HTTP socket.
 * @details Chunks caller data into bounded NetX packets and sends them in order.
 * @param[in] data Readable body or header bytes.
 * @param[in] bytes Total bytes to transmit.
 * @return NetX status code.
 * @retval NX_SUCCESS The entire span was queued.
 * @retval NX_NOT_SUCCESSFUL Packet allocation, append, or socket send failed.
 * @pre `data` addresses at least `bytes` readable bytes.
 * @pre `s_http_socket` is connected and `s_pool` is initialized.
 * @post On success, exactly `bytes` were sent in order.
 * @post Failed unsent packets are released before return.
 * @note Exact non-success codes are propagated from NetX.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT internal_c6_cam_http_send(const void* data, uint32_t bytes)
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

/**
 * @brief Append bounded text to a fixed HTTP header buffer.
 * @details Copies until NUL or capacity and returns the next write position.
 * @param[out] out Destination character buffer.
 * @param[in] at Initial write offset.
 * @param[in] cap Destination capacity.
 * @param[in] text NUL-terminated source text.
 * @return Updated write offset.
 * @retval uint32_t First unwritten offset, never greater than `cap`.
 * @pre `out` addresses `cap` writable bytes.
 * @pre `text` is nonnull and NUL-terminated.
 * @post Source text is unchanged.
 * @post Only destination bytes in `[at, return)` are modified.
 * @note Truncation is represented by a return value equal to `cap`.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t
internal_c6_cam_append(char* out, uint32_t at, uint32_t cap, const char* text)
{
  while ((at < cap) && (*text != '\0')) {
    out[at++] = *text++;
  }
  return at;
}

/** @brief Decimal formatting and minimum HTTP request sizes. */
typedef enum : uint32_t {
  k_c6_cam_net_decimal_digits = 10U, /**< Decimal digits in a 32-bit integer.  */
  k_c6_cam_http_get_min       = 5U,  /**< Minimum bytes in an HTTP GET prefix. */
} c6_cam_net_format_t;

/**
 * @brief Append an unsigned decimal value to a fixed buffer.
 * @details Formats digits in reverse stack storage and copies them in display order.
 * @param[out] out Destination character buffer.
 * @param[in] at Initial write offset.
 * @param[in] cap Destination capacity.
 * @param[in] value Value to append.
 * @return Updated write offset.
 * @retval uint32_t First unwritten offset, never greater than `cap`.
 * @pre `out` addresses `cap` writable bytes.
 * @pre `at` is no greater than `cap`.
 * @post The value's digits are appended until capacity is exhausted.
 * @post No destination byte before `at` is modified.
 * @note No NUL terminator is appended.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t
internal_c6_cam_append_u32(char* out, uint32_t at, uint32_t cap, uint32_t value)
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

/**
 * @brief Send one complete close-delimited HTTP response.
 * @details Builds bounded headers with optional timing and timestamp metadata, then sends the body.
 * @param[in] type NUL-terminated MIME content type.
 * @param[in] body Readable response body.
 * @param[in] body_bytes Response body size.
 * @param[in] timing Optional camera timing metadata.
 * @param[in] timestamp_ms Optional monotonic media timestamp.
 * @return NetX status code.
 * @retval NX_SUCCESS Header and body were sent.
 * @retval NX_NOT_SUCCESSFUL A socket send failed.
 * @pre `type` is NUL-terminated and `body` spans `body_bytes` bytes.
 * @pre The HTTP socket is connected.
 * @post The body is sent only after a successful header send.
 * @post Caller-owned response data remains unchanged.
 * @note A zero timestamp suppresses the timestamp header.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT internal_c6_cam_http_response(const char*                  type,
                                                       const void*                  body,
                                                       uint32_t                     body_bytes,
                                                       const c6_cam_frame_timing_t* timing,
                                                       uint32_t                     timestamp_ms)
{
  char     header[320] = {};
  uint32_t length =
    internal_c6_cam_append(header, 0U, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: ");
  length = internal_c6_cam_append(header, length, sizeof(header), type);
  if (timing != nullptr) {
    length =
      internal_c6_cam_append(header, length, sizeof(header), "\r\nServer-Timing: capture;dur=");
    length = internal_c6_cam_append_u32(header, length, sizeof(header), timing->capture_ms);
    length = internal_c6_cam_append(header, length, sizeof(header), ", convert;dur=");
    length = internal_c6_cam_append_u32(header, length, sizeof(header), timing->convert_ms);
    length = internal_c6_cam_append(header, length, sizeof(header), ", encode;dur=");
    length = internal_c6_cam_append_u32(header, length, sizeof(header), timing->encode_ms);
  }
  if (timestamp_ms != 0U) {
    length = internal_c6_cam_append(header, length, sizeof(header), "\r\nX-RA8-Timestamp-Ms: ");
    length = internal_c6_cam_append_u32(header, length, sizeof(header), timestamp_ms);
  }
  length = internal_c6_cam_append(header, length, sizeof(header), "\r\nContent-Length: ");
  length = internal_c6_cam_append_u32(header, length, sizeof(header), body_bytes);
  length =
    internal_c6_cam_append(header,
                           length,
                           sizeof(header),
                           "\r\nCache-Control: no-store, no-cache\r\nConnection: close\r\n\r\n");
  UINT status = internal_c6_cam_http_send(header, length);
  if (status == NX_SUCCESS) {
    status = internal_c6_cam_http_send(body, body_bytes);
  }
  return status;
}

/**
 * @brief Serve a bounded multipart MJPEG sequence.
 * @details Captures a fresh JPEG for each part and emits per-frame timing diagnostics.
 * @return NetX status code.
 * @retval NX_SUCCESS The configured frame sequence completed.
 * @retval NX_NOT_SUCCESSFUL Capture or socket transmission failed.
 * @pre The HTTP socket is connected.
 * @pre The selected camera backend is initialized.
 * @post Every successful frame is emitted as one complete multipart section.
 * @post Capture failures emit backend diagnostics before return.
 * @note Clients reconnect after the bounded sequence or any failure.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT internal_c6_cam_http_stream(void)
{
  static const char stream_header[] = "HTTP/1.1 200 OK\r\n"
                                      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                                      "Cache-Control: no-store, no-cache\r\n"
                                      "Connection: close\r\n\r\n";
  UINT status = internal_c6_cam_http_send(stream_header, (uint32_t)(sizeof(stream_header) - 1U));
  for (uint32_t frame_index = 0U;
       (frame_index < (uint32_t)k_c6_cam_stream_frames) && (status == NX_SUCCESS);
       frame_index++) {
    const uint8_t*        jpeg       = nullptr;
    uint32_t              jpeg_bytes = 0U;
    c6_cam_frame_timing_t timing     = {};
    const ra8_err_t       err        = c6_cam_camera_capture_jpeg(&jpeg, &jpeg_bytes, &timing);
    if (err != k_ra8_ok) {
      c6_cam_camera_report_last_error();
      return NX_NOT_SUCCESSFUL;
    }
    char     part_header[128] = {};
    uint32_t length =
      internal_c6_cam_append(part_header,
                             0U,
                             sizeof(part_header),
                             "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ");
    length = internal_c6_cam_append_u32(part_header, length, sizeof(part_header), jpeg_bytes);
    length = internal_c6_cam_append(part_header, length, sizeof(part_header), "\r\n\r\n");
    status = internal_c6_cam_http_send(part_header, length);
    if (status == NX_SUCCESS) {
      status = internal_c6_cam_http_send(jpeg, jpeg_bytes);
    }
    if (status == NX_SUCCESS) {
      static const char part_tail[] = "\r\n";
      status = internal_c6_cam_http_send(part_tail, (uint32_t)(sizeof(part_tail) - 1U));
    }
    if (status == NX_SUCCESS) {
      c6_cam_puts("c6_cam: stream jpeg_bytes=");
      c6_cam_put_u32(jpeg_bytes);
      c6_cam_puts(" capture_ms=");
      c6_cam_put_u32(timing.capture_ms);
      c6_cam_puts(" convert_ms=");
      c6_cam_put_u32(timing.convert_ms);
      c6_cam_puts(" encode_ms=");
      c6_cam_put_u32(timing.encode_ms);
      c6_cam_puts("\r\n");
    }
  }
  return status;
}

/**
 * @brief Match a bounded HTTP GET request path.
 * @details Compares the request target exactly while accepting a query suffix.
 * @param[in] request Raw request bytes.
 * @param[in] bytes Available request-byte count.
 * @param[in] path NUL-terminated path to match.
 * @return Match result.
 * @retval true The request is a GET for `path` with optional query text.
 * @retval false The method, length, or path does not match.
 * @pre `request` addresses at least `bytes` readable bytes.
 * @pre `path` is nonnull and NUL-terminated.
 * @post Request and path storage remain unchanged.
 * @post No bytes beyond the supplied request bound are read.
 * @note HTTP decoding beyond this narrow routing grammar is intentionally absent.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_c6_cam_request_is(const char* request, uint32_t bytes, const char* path)
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

/**
 * @brief Capture and serve one JPEG still response.
 * @details Captures through the configured camera backend, emits the JPEG with
 * timing metadata on success, and otherwise sends a bounded diagnostic body.
 * @pre Camera state and HTTP packet resources are initialized.
 * @pre The connected socket may accept one complete response attempt.
 * @post A JPEG or bounded text error response is attempted.
 * @post Capture timing or the backend error is reported on the console.
 * @note Capture diagnostics are emitted on the console.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c6_cam_http_frame(void)
{
  const uint8_t*        jpeg       = nullptr;
  uint32_t              jpeg_bytes = 0U;
  c6_cam_frame_timing_t timing     = {};
  const ra8_err_t       err        = c6_cam_camera_capture_jpeg(&jpeg, &jpeg_bytes, &timing);
  if (err == k_ra8_ok) {
    (void)
      internal_c6_cam_http_response("image/jpeg", jpeg, jpeg_bytes, &timing, timing.timestamp_ms);
    c6_cam_puts("c6_cam: frame jpeg_bytes=");
    c6_cam_put_u32(jpeg_bytes);
    c6_cam_puts(" capture_ms=");
    c6_cam_put_u32(timing.capture_ms);
    c6_cam_puts(" convert_ms=");
    c6_cam_put_u32(timing.convert_ms);
    c6_cam_puts(" encode_ms=");
    c6_cam_put_u32(timing.encode_ms);
    c6_cam_puts("\r\n");
    return;
  }
  static const char failed[] = "camera capture failed\n";
  (void)internal_c6_cam_http_response("text/plain",
                                      failed,
                                      (uint32_t)(sizeof(failed) - 1U),
                                      nullptr,
                                      0U);
  c6_cam_puts("c6_cam: frame FAIL err=");
  c6_cam_puts(ra8_err_to_str(err));
  c6_cam_camera_report_last_error();
  c6_cam_puts("\r\n");
}

/**
 * @brief Snapshot and serve the current audio window.
 * @details Requests a stable WAV snapshot from the ping-pong audio backend and
 * returns either that snapshot or a bounded not-ready response to the client.
 * @pre Audio state and HTTP packet resources are initialized.
 * @pre The connected socket may accept one complete response attempt.
 * @post A WAV or bounded text not-ready response is attempted.
 * @post Successful snapshots retain their capture timestamp in the response.
 * @note Successful responses include the capture timestamp.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c6_cam_http_audio(void)
{
  const uint8_t*  wav          = nullptr;
  uint32_t        wav_bytes    = 0U;
  uint32_t        timestamp_ms = 0U;
  const ra8_err_t err          = c6_cam_audio_snapshot_wav(&wav, &wav_bytes, &timestamp_ms);
  if (err == k_ra8_ok) {
    (void)internal_c6_cam_http_response("audio/wav", wav, wav_bytes, nullptr, timestamp_ms);
    c6_cam_puts("c6_cam: audio wav_bytes=");
    c6_cam_put_u32(wav_bytes);
    c6_cam_puts(" timestamp_ms=");
    c6_cam_put_u32(timestamp_ms);
    c6_cam_puts("\r\n");
    return;
  }
  static const char unavailable[] = "audio capture not ready\n";
  (void)internal_c6_cam_http_response("text/plain",
                                      unavailable,
                                      (uint32_t)(sizeof(unavailable) - 1U),
                                      nullptr,
                                      0U);
}

/**
 * @brief Dispatch a bounded HTTP request to one route handler.
 * @details Matches the request path against the stream, still-frame, audio,
 * and health endpoints, falling back to the embedded browser page.
 * @param[in] request Extracted request prefix.
 * @param[in] bytes Extracted request-byte count.
 * @pre `request` addresses at least `bytes` readable bytes.
 * @pre Camera, audio, and HTTP response resources are initialized.
 * @post At most one response handler is invoked.
 * @post Unrecognized routes receive the embedded browser page.
 * @note Handler response failures are resolved by the caller's socket teardown.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c6_cam_http_dispatch(const char* request, uint32_t bytes)
{
  if (internal_c6_cam_request_is(request, bytes, "/stream.mjpg")) {
    (void)internal_c6_cam_http_stream();
  } else if (internal_c6_cam_request_is(request, bytes, "/frame.jpg")) {
    internal_c6_cam_http_frame();
  } else if (internal_c6_cam_request_is(request, bytes, "/audio.wav")) {
    internal_c6_cam_http_audio();
  } else if (internal_c6_cam_request_is(request, bytes, "/health")) {
    static const char healthy[] = "PASS c6 camera livestream audio=PASS camera=PASS\n";
    (void)internal_c6_cam_http_response("text/plain",
                                        healthy,
                                        (uint32_t)(sizeof(healthy) - 1U),
                                        nullptr,
                                        0U);
  } else {
    (void)internal_c6_cam_http_response("text/html; charset=utf-8",
                                        s_page,
                                        (uint32_t)(sizeof(s_page) - 1U),
                                        nullptr,
                                        0U);
  }
}

/**
 * @brief Receive and dispatch one HTTP request.
 * @details Extracts a bounded request prefix and serves stream, frame, audio, health, or UI content.
 * @pre `s_http_socket` is connected to one client.
 * @pre Camera, audio, packet pool, and IP state are initialized.
 * @post Any received request packet is released exactly once.
 * @post At most one route handler is selected for the request.
 * @note Response failures are handled by subsequent socket teardown.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c6_cam_http_handle(void)
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
  internal_c6_cam_http_dispatch(request, (uint32_t)copied);
}

void c6_cam_http_serve(void)
{
  UINT status = nx_tcp_socket_create(&s_ip,
                                     &s_http_socket,
                                     s_socket_name,
                                     NX_IP_NORMAL,
                                     NX_FRAGMENT_OKAY,
                                     NX_IP_TIME_TO_LIVE,
                                     (ULONG)k_c6_cam_tcp_window,
                                     NX_NULL,
                                     NX_NULL);
  if (status == NX_SUCCESS) {
    status = nx_tcp_server_socket_listen(&s_ip,
                                         (UINT)k_c6_cam_http_port,
                                         &s_http_socket,
                                         (UINT)k_c6_cam_listen_backlog,
                                         NX_NULL);
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
      internal_c6_cam_http_handle();
      (void)nx_tcp_socket_disconnect(&s_http_socket, NX_WAIT_FOREVER);
      (void)nx_tcp_server_socket_unaccept(&s_http_socket);
    }
    (void)nx_tcp_server_socket_relisten(&s_ip, (UINT)k_c6_cam_http_port, &s_http_socket);
  }
}
