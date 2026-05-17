/**
 * @file ra_da16600.c
 * @brief Renesas DA16600 Wi-Fi + BLE combo module driver implementation
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements the ::ra_da16600.h public API on top of the generic
 * @c ra_modem_at AT-command transport. Each public function formats
 * one DA16600 AT command, hands it to @c ra_modem_at_send_cmd or
 * @c ra_modem_at_send_cmd_capture, and parses the captured payload.
 *
 * Citations to UM-WI-046 (Renesas "DA16200_DA16600 AT Command Set")
 * are inline above every command-emitting helper.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra_da16600.h"

#include <stddef.h>
#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_modem_at.h"

/**
 * @def RA_DA16600_TAG
 * @brief Logging tag used for every ``ra_log_error`` call in this TU.
 */
#define RA_DA16600_TAG "DA16600"

/* =============================================================================
 * Module-static state
 * =============================================================================
 */

/**
 * @brief Compile-time sized stack-local capture buffers used in this TU.
 *
 * @details
 * All sizes are typed enums so the buffer-size sweep tool can prove
 * each function's local-frame budget without preprocessor expansion.
 */
typedef enum : uint16_t {
  k_ra_da16600_cmd_buf_bytes     = 96U,  /**< Per-command AT line buffer (TX). */
  k_ra_da16600_capture_buf_bytes = 256U, /**< Per-command response capture. */
  k_ra_da16600_payload_max_bytes = 1460U, /**< One TCP MSS. UM-WI-046 5.2.5. */
} ra_da16600_internal_caps_t;

/**
 * @brief Persistent driver state.
 *
 * @details Single instance per build -- the DA16600 module-side state
 * machine is single-owner and not thread-safe.
 */
typedef struct {
  uint8_t initialized; /**< 1 once ::ra_da16600_init returns ::k_ra_ok. */
} ra_da16600_state_t;

/**
 * @brief Module state singleton.
 */
static ra_da16600_state_t s_da;

/* =============================================================================
 * Tiny string helpers (no libc dependency on the target)
 * =============================================================================
 */

/**
 * @brief NUL-terminated string length, capped at @c UINT16_MAX.
 *
 * @param[in] s NUL-terminated string (must be non-NULL).
 * @return Length in bytes.
 *
 * @pre @p s is non-NULL and NUL-terminated.
 * @post No state mutated.
 * @note Pure function; safe to call concurrently.
 * @since 0.1.0
 */
static uint16_t internal_str_len(const char* s)
{
  uint16_t i = 0U;
  while ((i < (uint16_t)UINT16_MAX) && (s[i] != '\0')) {
    ++i;
  }
  return i;
}

/**
 * @brief Append @p src to @p dst, NUL-terminate, bounded by @p cap.
 *
 * @param[in,out] dst Destination buffer (NUL-terminated on exit).
 * @param[in]     cap Capacity of @p dst including NUL.
 * @param[in,out] off In: current write offset. Out: updated offset.
 * @param[in]     src NUL-terminated string to append.
 *
 * @return 1 on success, 0 if appending would overflow.
 *
 * @pre @p dst, @p off, @p src non-NULL.
 * @pre @p cap >= 1.
 * @post On success, ``dst[*off] == '\0'``.
 * @post On overflow, ``dst[cap-1] == '\0'`` and result is 0.
 * @since 0.1.0
 */
static uint8_t internal_strcat_bounded(char* dst, size_t cap, size_t* off, const char* src)
{
  size_t   i  = *off;
  uint16_t sl = internal_str_len(src);
  if ((i + (size_t)sl + 1U) > cap) {
    if (cap > 0U) {
      dst[cap - 1U] = '\0';
    }
    return 0U;
  }
  for (uint16_t k = 0U; k < sl; ++k) {
    dst[i] = src[k];
    ++i;
  }
  dst[i] = '\0';
  *off   = i;
  return 1U;
}

/**
 * @brief Format a decimal uint32_t into @p dst, NUL-terminated.
 *
 * @details Avoids @c snprintf so the driver stays libc-free for the
 * target build. Writes at most 11 bytes ("4294967295" + NUL).
 *
 * @param[out] dst    Destination; must hold at least 11 bytes.
 * @param[in]  value  Value to format.
 *
 * @pre @p dst is non-NULL.
 * @post @c dst is NUL-terminated.
 * @since 0.1.0
 */
static void internal_format_u32(char* dst, uint32_t value)
{
  /* Worst case "4294967295" = 10 digits. */
  char     tmp[11];
  uint8_t  ti  = 0U;
  uint32_t v   = value;
  if (v == 0U) {
    dst[0] = '0';
    dst[1] = '\0';
    return;
  }
  while ((v > 0U) && (ti < 10U)) {
    tmp[ti] = (char)('0' + (uint8_t)(v % 10U));
    v /= 10U;
    ++ti;
  }
  /* Reverse into dst. */
  uint8_t di = 0U;
  while (ti > 0U) {
    --ti;
    dst[di] = tmp[ti];
    ++di;
  }
  dst[di] = '\0';
}

/**
 * @brief Count non-empty slash-separated entries in a string.
 *
 * @details
 * The DA16600 ``+WFSCAN:`` response packs every BSSID into one line
 * separated by ``\x01`` or ``/`` depending on firmware revision. We
 * accept both. UM-WI-046 section 4.1 documents the slash-separated
 * form for FW 3.2+ and the SOH-separated form for legacy 3.0 builds.
 *
 * @param[in] s NUL-terminated input string.
 * @return Entry count (0 if empty / NULL).
 * @since 0.1.0
 */
static uint16_t internal_count_scan_entries(const char* s)
{
  if (s == nullptr) {
    return 0U;
  }
  /* An empty payload reports zero networks. */
  if (s[0] == '\0') {
    return 0U;
  }
  uint16_t count = 1U;
  uint16_t i     = 0U;
  while (s[i] != '\0') {
    if ((s[i] == '/') || (s[i] == '\x01')) {
      ++count;
    }
    ++i;
  }
  return count;
}

/**
 * @brief Verify driver was initialized before any command call.
 *
 * @return ::k_ra_ok if initialized, ::k_ra_err_not_initialized otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_require_init(void)
{
  if (s_da.initialized == 0U) {
    ra_log_error(RA_DA16600_TAG, "not initialized");
    return k_ra_err_not_initialized;
  }
  return k_ra_ok;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/* UM-WI-046 section 2.1 "Basic AT Commands": bare ``AT`` returns OK. */
ra_err_t ra_da16600_init(const ra_da16600_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, RA_DA16600_TAG, "cfg");
  RA_CHECK_NULL_PTR(cfg->line_buf, RA_DA16600_TAG, "cfg->line_buf");
  if (cfg->line_buf_len < (uint16_t)k_ra_da16600_min_line_buf) {
    ra_log_error(RA_DA16600_TAG, "line_buf_len below floor");
    return k_ra_err_invalid_size;
  }
  if ((cfg->io.tx_byte == nullptr) || (cfg->io.rx_byte == nullptr)
      || (cfg->io.now_ms == nullptr)) {
    ra_log_error(RA_DA16600_TAG, "io callbacks must be non-NULL");
    return k_ra_err_null_ptr;
  }

  /* Reset state before any IO -- on retry after a previous failure we
   * want a clean slate. */
  s_da.initialized = 0U;

  ra_modem_at_cfg_t at_cfg = {
    .io                 = cfg->io,
    .line_buf           = cfg->line_buf,
    .line_buf_len       = cfg->line_buf_len,
    .default_timeout_ms = cfg->default_timeout_ms,
  };
  ra_err_t err = ra_modem_at_init(&at_cfg);
  if (err != k_ra_ok) {
    ra_log_error(RA_DA16600_TAG, "modem_at init failed");
    return err;
  }

  /* Probe: bare "AT" -- UM-WI-046 section 2.1. */
  err = ra_modem_at_send_cmd("AT", nullptr, (uint16_t)k_ra_da16600_timeout_probe_ms);
  if (err != k_ra_ok) {
    ra_log_error(RA_DA16600_TAG, "AT probe failed");
    return err;
  }

  s_da.initialized = 1U;
  return k_ra_ok;
}

/* =============================================================================
 * Wi-Fi
 * =============================================================================
 */

/* UM-WI-046 section 4.1 "Wi-Fi Scan": AT+WFSCAN returns one
 * +WFSCAN:<list> line followed by OK. */
ra_err_t ra_da16600_wifi_scan(uint16_t* out_count)
{
  RA_CHECK_NULL_PTR(out_count, RA_DA16600_TAG, "out_count");
  *out_count   = 0U;
  ra_err_t err = internal_require_init();
  if (err != k_ra_ok) {
    return err;
  }

  char capture[k_ra_da16600_capture_buf_bytes];
  capture[0] = '\0';
  err        = ra_modem_at_send_cmd_capture(
    "AT+WFSCAN", capture, (size_t)k_ra_da16600_capture_buf_bytes, (uint16_t)k_ra_da16600_timeout_scan_ms);
  if (err != k_ra_ok) {
    return err;
  }

  /* Locate the "+WFSCAN:" prefix and count entries after it. */
  uint16_t i = 0U;
  while ((capture[i] != '\0') && (capture[i] != ':')) {
    ++i;
  }
  if (capture[i] == ':') {
    ++i;
    /* Skip an optional leading space the firmware sometimes inserts. */
    if (capture[i] == ' ') {
      ++i;
    }
    *out_count = internal_count_scan_entries(&capture[i]);
  } else {
    *out_count = 0U;
  }
  return k_ra_ok;
}

/* UM-WI-046 section 4.5 "Connect / Disconnect AP": AT+WFJAP. */
ra_err_t ra_da16600_wifi_connect(const char* ssid,
                                  const char* passkey,
                                  char*       out_ip_str,
                                  size_t      ip_str_len)
{
  RA_CHECK_NULL_PTR(ssid, RA_DA16600_TAG, "ssid");
  RA_CHECK_NULL_PTR(passkey, RA_DA16600_TAG, "passkey");
  RA_CHECK_NULL_PTR(out_ip_str, RA_DA16600_TAG, "out_ip_str");
  if (ip_str_len < (size_t)k_ra_da16600_ip_str_len) {
    ra_log_error(RA_DA16600_TAG, "ip_str_len too small");
    return k_ra_err_invalid_size;
  }
  out_ip_str[0] = '\0';

  uint16_t ssid_len = internal_str_len(ssid);
  uint16_t pk_len   = internal_str_len(passkey);
  if ((ssid_len == 0U) || (ssid_len >= (uint16_t)k_ra_da16600_ssid_max_len)
      || (pk_len >= (uint16_t)k_ra_da16600_passkey_max_len)) {
    ra_log_error(RA_DA16600_TAG, "ssid/passkey length invalid");
    return k_ra_err_invalid_size;
  }

  ra_err_t err = internal_require_init();
  if (err != k_ra_ok) {
    return err;
  }

  /* Build the AT command into a stack buffer:
   *   AT+WFJAP=<ssid>,<security>,<key>
   * Security mode 4 == WPA2-PSK per UM-WI-046 Table 4.5-1. */
  char   cmd[k_ra_da16600_cmd_buf_bytes];
  size_t off = 0U;
  cmd[0]     = '\0';
  if ((internal_strcat_bounded(cmd, sizeof cmd, &off, "AT+WFJAP=") == 0U)
      || (internal_strcat_bounded(cmd, sizeof cmd, &off, ssid) == 0U)
      || (internal_strcat_bounded(cmd, sizeof cmd, &off, ",4,") == 0U)
      || (internal_strcat_bounded(cmd, sizeof cmd, &off, passkey) == 0U)) {
    ra_log_error(RA_DA16600_TAG, "WFJAP command overflows cmd buffer");
    return k_ra_err_invalid_size;
  }

  char capture[k_ra_da16600_capture_buf_bytes];
  capture[0] = '\0';
  err        = ra_modem_at_send_cmd_capture(
    cmd, capture, (size_t)k_ra_da16600_capture_buf_bytes, (uint16_t)k_ra_da16600_timeout_connect_ms);
  if (err != k_ra_ok) {
    return err;
  }

  /* Parse "+WFJAP:1,<ssid>,<ip>" -- pull the IP after the third comma.
   * UM-WI-046 section 4.5 documents the URC layout. */
  uint16_t i       = 0U;
  uint16_t commas  = 0U;
  while ((capture[i] != '\0') && (commas < 2U)) {
    if (capture[i] == ',') {
      ++commas;
    }
    ++i;
  }
  if ((capture[i] == '\0') || (commas != 2U)) {
    ra_log_error(RA_DA16600_TAG, "WFJAP response malformed");
    return k_ra_err_hw_error;
  }
  /* Copy IP until comma / newline / NUL. */
  size_t   oi = 0U;
  while ((capture[i] != '\0') && (capture[i] != ',') && (capture[i] != '\r')
         && (capture[i] != '\n') && (oi + 1U < ip_str_len)) {
    out_ip_str[oi] = capture[i];
    ++oi;
    ++i;
  }
  out_ip_str[oi] = '\0';
  if (oi == 0U) {
    ra_log_error(RA_DA16600_TAG, "WFJAP IP empty");
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

/* UM-WI-046 section 4.5: AT+WFQAP disassociates. */
ra_err_t ra_da16600_wifi_disconnect(void)
{
  ra_err_t err = internal_require_init();
  if (err != k_ra_ok) {
    return err;
  }
  return ra_modem_at_send_cmd("AT+WFQAP", nullptr, (uint16_t)k_ra_da16600_timeout_probe_ms);
}

/* =============================================================================
 * TCP sockets
 * =============================================================================
 */

/* UM-WI-046 section 5.2.3 (TRTS: listen) / 5.2.4 (TRTC: client). */
ra_err_t ra_da16600_tcp_open(ra_da16600_socket_role_t role,
                              const char*              remote_ip,
                              uint16_t                 port,
                              ra_da16600_socket_t*     out_socket)
{
  RA_CHECK_NULL_PTR(out_socket, RA_DA16600_TAG, "out_socket");
  *out_socket = 0U;

  if ((role != k_ra_da16600_socket_listen) && (role != k_ra_da16600_socket_connect)) {
    ra_log_error(RA_DA16600_TAG, "invalid role");
    return k_ra_err_invalid_arg;
  }
  if (role == k_ra_da16600_socket_connect) {
    RA_CHECK_NULL_PTR(remote_ip, RA_DA16600_TAG, "remote_ip");
  }

  ra_err_t err = internal_require_init();
  if (err != k_ra_ok) {
    return err;
  }

  char   cmd[k_ra_da16600_cmd_buf_bytes];
  size_t off = 0U;
  cmd[0]     = '\0';
  char port_str[11];
  internal_format_u32(port_str, (uint32_t)port);

  if (role == k_ra_da16600_socket_listen) {
    if ((internal_strcat_bounded(cmd, sizeof cmd, &off, "AT+TRTS=") == 0U)
        || (internal_strcat_bounded(cmd, sizeof cmd, &off, port_str) == 0U)) {
      ra_log_error(RA_DA16600_TAG, "TRTS command overflow");
      return k_ra_err_invalid_size;
    }
  } else {
    if ((internal_strcat_bounded(cmd, sizeof cmd, &off, "AT+TRTC=") == 0U)
        || (internal_strcat_bounded(cmd, sizeof cmd, &off, remote_ip) == 0U)
        || (internal_strcat_bounded(cmd, sizeof cmd, &off, ",") == 0U)
        || (internal_strcat_bounded(cmd, sizeof cmd, &off, port_str) == 0U)) {
      ra_log_error(RA_DA16600_TAG, "TRTC command overflow");
      return k_ra_err_invalid_size;
    }
  }

  char capture[k_ra_da16600_capture_buf_bytes];
  capture[0] = '\0';
  err        = ra_modem_at_send_cmd_capture(
    cmd, capture, (size_t)k_ra_da16600_capture_buf_bytes, (uint16_t)k_ra_da16600_timeout_socket_ms);
  if (err != k_ra_ok) {
    return err;
  }

  /* Both TRTS and TRTC report the allocated cid in "+TRTS:<cid>" or
   * "+TRTC:<cid>" (UM-WI-046 5.2.3 / 5.2.4). Locate the first digit
   * after ':' and stop at the first non-digit. */
  uint16_t i = 0U;
  while ((capture[i] != '\0') && (capture[i] != ':')) {
    ++i;
  }
  if (capture[i] != ':') {
    /* No cid line; if the module replied bare OK assume cid 0. This
     * matches several DA16600 firmware revisions that defer cid
     * publication to the first inbound packet. */
    *out_socket = 0U;
    return k_ra_ok;
  }
  ++i;
  if (capture[i] == ' ') {
    ++i;
  }
  uint32_t cid = 0U;
  uint8_t  saw = 0U;
  while ((capture[i] >= '0') && (capture[i] <= '9')) {
    cid = (cid * 10U) + (uint32_t)(capture[i] - '0');
    ++i;
    saw = 1U;
  }
  if (saw == 0U) {
    ra_log_error(RA_DA16600_TAG, "cid parse failed");
    return k_ra_err_hw_error;
  }
  *out_socket = (ra_da16600_socket_t)cid;
  return k_ra_ok;
}

/* UM-WI-046 section 5.2.5 "Send Data on TCP": AT+TRDTS=<cid>,<len>,<data>. */
ra_err_t ra_da16600_tcp_send(ra_da16600_socket_t sock, const uint8_t* data, size_t len)
{
  if (len == 0U) {
    ra_log_error(RA_DA16600_TAG, "tcp_send len=0");
    return k_ra_err_invalid_size;
  }
  if (len > (size_t)k_ra_da16600_payload_max_bytes) {
    ra_log_error(RA_DA16600_TAG, "tcp_send len > MSS");
    return k_ra_err_invalid_size;
  }
  RA_CHECK_NULL_PTR(data, RA_DA16600_TAG, "data");

  ra_err_t err = internal_require_init();
  if (err != k_ra_ok) {
    return err;
  }

  /* Build "AT+TRDTS=<cid>,<len>," prefix. The byte payload itself is
   * appended raw (length-prefixed framing -- UM-WI-046 5.2.5). */
  char   cmd[k_ra_da16600_cmd_buf_bytes];
  size_t off = 0U;
  cmd[0]     = '\0';
  char num[11];

  internal_format_u32(num, (uint32_t)sock);
  if ((internal_strcat_bounded(cmd, sizeof cmd, &off, "AT+TRDTS=") == 0U)
      || (internal_strcat_bounded(cmd, sizeof cmd, &off, num) == 0U)
      || (internal_strcat_bounded(cmd, sizeof cmd, &off, ",") == 0U)) {
    ra_log_error(RA_DA16600_TAG, "TRDTS command overflow");
    return k_ra_err_invalid_size;
  }
  internal_format_u32(num, (uint32_t)len);
  if ((internal_strcat_bounded(cmd, sizeof cmd, &off, num) == 0U)
      || (internal_strcat_bounded(cmd, sizeof cmd, &off, ",") == 0U)) {
    ra_log_error(RA_DA16600_TAG, "TRDTS command overflow");
    return k_ra_err_invalid_size;
  }

  /* The DA16600 expects the binary payload to follow the comma-list
   * head immediately. We append it to the command buffer if it fits;
   * otherwise we'd need a stream-mode write (not exposed by the
   * current ra_modem_at). The payload-overflow path is signalled
   * back to the caller so they can split into smaller chunks. */
  if ((off + len + 1U) > sizeof cmd) {
    ra_log_error(RA_DA16600_TAG, "payload exceeds AT-line buffer");
    return k_ra_err_invalid_size;
  }
  for (size_t i = 0U; i < len; ++i) {
    cmd[off] = (char)data[i];
    ++off;
  }
  cmd[off] = '\0';

  return ra_modem_at_send_cmd(cmd, nullptr, (uint16_t)k_ra_da16600_timeout_socket_ms);
}

/* UM-WI-046 section 5.2.6 "Receive Data on TCP": +TRDTC:<cid>,<len>,<data>
 * arrives asynchronously. We block-poll the AT pipe until the URC is
 * captured or the caller-specified timeout fires. */
ra_err_t ra_da16600_tcp_recv(ra_da16600_socket_t sock,
                              uint8_t*            buf,
                              size_t              cap,
                              size_t*             out_len,
                              uint16_t            timeout_ms)
{
  (void)sock; /* Recv buffer is socket-multiplexed by the module; the
                 caller treats us as a flat byte stream for now. */
  RA_CHECK_NULL_PTR(buf, RA_DA16600_TAG, "buf");
  RA_CHECK_NULL_PTR(out_len, RA_DA16600_TAG, "out_len");
  *out_len = 0U;
  if (cap == 0U) {
    ra_log_error(RA_DA16600_TAG, "tcp_recv cap=0");
    return k_ra_err_invalid_size;
  }

  ra_err_t err = internal_require_init();
  if (err != k_ra_ok) {
    return err;
  }

  /* Issue an empty "AT" with +TRDTC as the expected prefix; the AT
   * driver will pump RX bytes until it sees a line whose first 7
   * characters are "+TRDTC:" or the timeout fires. We capture into
   * a stack buffer and then extract the binary payload that follows
   * the third comma. */
  char capture[k_ra_da16600_capture_buf_bytes];
  capture[0]      = '\0';
  uint16_t to_use = (timeout_ms == 0U) ? (uint16_t)k_ra_da16600_timeout_socket_ms : timeout_ms;
  err = ra_modem_at_send_cmd_capture("AT", capture, (size_t)k_ra_da16600_capture_buf_bytes, to_use);
  if (err != k_ra_ok) {
    return err;
  }

  /* Find "+TRDTC:" line. */
  uint16_t i = 0U;
  while ((capture[i] != '\0') && (capture[i] != ':')) {
    ++i;
  }
  if (capture[i] != ':') {
    return k_ra_err_hw_timeout;
  }
  /* Skip past two commas to reach the binary payload. */
  uint16_t commas = 0U;
  while ((capture[i] != '\0') && (commas < 2U)) {
    if (capture[i] == ',') {
      ++commas;
    }
    ++i;
  }
  if (capture[i] == '\0') {
    return k_ra_err_hw_error;
  }
  /* Copy until newline or NUL, bounded by @p cap. */
  size_t oi = 0U;
  while ((capture[i] != '\0') && (capture[i] != '\r') && (capture[i] != '\n') && (oi < cap)) {
    buf[oi] = (uint8_t)capture[i];
    ++oi;
    ++i;
  }
  *out_len = oi;
  return k_ra_ok;
}

/* UM-WI-046 section 5.2.7 "Terminate Session": AT+TRTRM=<cid>. */
ra_err_t ra_da16600_tcp_close(ra_da16600_socket_t sock)
{
  ra_err_t err = internal_require_init();
  if (err != k_ra_ok) {
    return err;
  }
  char   cmd[k_ra_da16600_cmd_buf_bytes];
  size_t off = 0U;
  cmd[0]     = '\0';
  char num[11];
  internal_format_u32(num, (uint32_t)sock);
  if ((internal_strcat_bounded(cmd, sizeof cmd, &off, "AT+TRTRM=") == 0U)
      || (internal_strcat_bounded(cmd, sizeof cmd, &off, num) == 0U)) {
    ra_log_error(RA_DA16600_TAG, "TRTRM command overflow");
    return k_ra_err_invalid_size;
  }
  return ra_modem_at_send_cmd(cmd, nullptr, (uint16_t)k_ra_da16600_timeout_socket_ms);
}

/* =============================================================================
 * BLE
 * =============================================================================
 */

/* UM-WI-046 section 7.4 "BLE Advertising Control": AT+BLEADVSTART. */
ra_err_t ra_da16600_ble_advertise_start(void)
{
  ra_err_t err = internal_require_init();
  if (err != k_ra_ok) {
    return err;
  }
  return ra_modem_at_send_cmd(
    "AT+BLEADVSTART", nullptr, (uint16_t)k_ra_da16600_timeout_ble_ms);
}

/* UM-WI-046 section 7.4: AT+BLEADVSTOP. */
ra_err_t ra_da16600_ble_advertise_stop(void)
{
  ra_err_t err = internal_require_init();
  if (err != k_ra_ok) {
    return err;
  }
  return ra_modem_at_send_cmd(
    "AT+BLEADVSTOP", nullptr, (uint16_t)k_ra_da16600_timeout_ble_ms);
}
