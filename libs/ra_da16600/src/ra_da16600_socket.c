/**
 * @file ra_da16600_socket.c
 * @brief DA16600 TCP-socket and BLE-advertising command implementations.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Second translation unit of the DA16600 driver. Implements the TCP
 * socket lifecycle (open / send / recv / close) and the BLE advertising
 * controls from the ::ra_da16600.h public API, splitting them out of
 * @c ra_da16600.c so each TU stays under the per-file line-count gate.
 * It shares the logging tag, buffer-sizing enums, and the bounded
 * string / init-gate helpers with @c ra_da16600.c via
 * @c ra_da16600_internal.h.
 *
 * Citations to UM-WI-046 (Renesas "DA16200_DA16600 AT Command Set")
 * are inline above every command-emitting helper.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_check.h"
#include "ra_da16600.h"
#include "ra_da16600_internal.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_modem_at.h"

/**
 * @brief Parse the ``+TRTS:<cid>`` / ``+TRTC:<cid>`` numeric tail.
 *
 * @details
 * UM-WI-046 sections 5.2.3 / 5.2.4 report the allocated cid as a
 * decimal integer immediately following a colon (with an optional
 * leading space). Some firmware revisions skip the cid line entirely
 * and just reply bare ``OK``; this helper signals that by returning
 * ::k_ra_ok with @c *out_cid == 0 so the caller can keep that
 * legacy-firmware contract.
 *
 * @param[in]  capture  Captured DA16600 response line.
 * @param[out] out_cid  Parsed cid (0 if no cid line present).
 *
 * @return ::ra_err_t
 * @retval k_ra_ok           Cid parsed (or absent, legacy firmware).
 * @retval k_ra_err_hw_error Cid colon present but no digits followed.
 *
 * @pre @p capture is non-NULL and NUL-terminated.
 * @pre @p out_cid is non-NULL.
 * @post @p *out_cid is in 0..255.
 * @post On success, no internal state is mutated.
 *
 * @note Not thread-safe; caller must serialise driver access.
 * @since 0.1.0
 */
static ra_err_t internal_parse_socket_cid(const char* capture, ra_da16600_socket_t* out_cid)
{
  uint16_t i = 0U;
  while (capture[i] != '\0') {
    if (capture[i] == ':') {
      break;
    }
    ++i;
  }
  if (capture[i] != ':') {
    /* No cid line; assume legacy firmware that defers cid publication. */
    *out_cid = 0U;
    return k_ra_ok;
  }
  ++i;
  if (capture[i] == ' ') {
    ++i;
  }
  uint32_t cid = 0U;
  uint8_t  saw = 0U;
  while (capture[i] >= '0') {
    if (capture[i] > '9') {
      break;
    }
    cid = (cid * (uint32_t)k_ra_da16600_decimal_base) + (uint32_t)(capture[i] - '0');
    ++i;
    saw = 1U;
  }
  if (saw == 0U) {
    ra_log_error(RA_DA16600_TAG, "cid parse failed");
    return k_ra_err_hw_error;
  }
  *out_cid = (ra_da16600_socket_t)cid;
  return k_ra_ok;
}

/** @brief Implementation of `ra_da16600_build_tcp_open_cmd()` -- bounded append
 *         of ``AT+TRTS=<port>`` (listen) or ``AT+TRTC=<ip>,<port>`` (connect). */
ra_err_t ra_da16600_build_tcp_open_cmd(char*                    cmd,
                                       size_t                   cmd_cap,
                                       ra_da16600_socket_role_t role,
                                       const char*              remote_ip,
                                       uint16_t                 port)
{
  size_t off                                = 0U;
  cmd[0]                                    = '\0';
  char port_str[k_ra_da16600_u32_str_bytes] = {};
  ra_da16600_format_u32(port_str, (uint32_t)port);

  if (role == k_ra_da16600_socket_listen) {
    if ((ra_da16600_strcat_bounded(cmd, cmd_cap, &off, "AT+TRTS=") == 0U) ||
        (ra_da16600_strcat_bounded(cmd, cmd_cap, &off, port_str) == 0U)) {
      /* "AT+TRTS="(8) + 5-digit port = max 13 bytes; 96-byte cmd buffer never fills. */
      ra_log_error(RA_DA16600_TAG, "TRTS command overflow"); /* GCOVR_EXCL_LINE */
      return k_ra_err_invalid_size;                          /* GCOVR_EXCL_LINE */
    }
    return k_ra_ok;
  }
  if ((ra_da16600_strcat_bounded(cmd, cmd_cap, &off, "AT+TRTC=") == 0U) ||
      (ra_da16600_strcat_bounded(cmd, cmd_cap, &off, remote_ip) == 0U) ||
      (ra_da16600_strcat_bounded(cmd, cmd_cap, &off, ",") == 0U) ||
      (ra_da16600_strcat_bounded(cmd, cmd_cap, &off, port_str) == 0U)) {
    ra_log_error(RA_DA16600_TAG, "TRTC command overflow");
    return k_ra_err_invalid_size;
  }
  return k_ra_ok;
}

/** @brief Implementation of `ra_da16600_build_trdts_header()` -- bounded append
 *         of the ``AT+TRDTS=<cid>,<len>,`` send-data header (no payload). */
ra_err_t ra_da16600_build_trdts_header(char*               cmd,
                                       size_t              cmd_cap,
                                       ra_da16600_socket_t sock,
                                       size_t              len,
                                       size_t*             out_off)
{
  size_t off = 0U;
  cmd[0]     = '\0';
  *out_off   = 0U;

  char num[k_ra_da16600_u32_str_bytes] = {};
  ra_da16600_format_u32(num, (uint32_t)sock);
  if ((ra_da16600_strcat_bounded(cmd, cmd_cap, &off, "AT+TRDTS=") == 0U) ||
      (ra_da16600_strcat_bounded(cmd, cmd_cap, &off, num) == 0U) ||
      (ra_da16600_strcat_bounded(cmd, cmd_cap, &off, ",") == 0U)) {
    /* "AT+TRDTS=" + 3-digit sock + "," = max 13 bytes; 96-byte buffer never fills. */
    ra_log_error(RA_DA16600_TAG, "TRDTS command overflow"); /* GCOVR_EXCL_LINE */
    return k_ra_err_invalid_size;                           /* GCOVR_EXCL_LINE */
  }
  ra_da16600_format_u32(num, (uint32_t)len);
  if ((ra_da16600_strcat_bounded(cmd, cmd_cap, &off, num) == 0U) ||
      (ra_da16600_strcat_bounded(cmd, cmd_cap, &off, ",") == 0U)) {
    /* len_str (4 digits) + "," = max 5 bytes; combined header max ~18 bytes < 96. */
    ra_log_error(RA_DA16600_TAG, "TRDTS command overflow"); /* GCOVR_EXCL_LINE */
    return k_ra_err_invalid_size;                           /* GCOVR_EXCL_LINE */
  }
  *out_off = off;
  return k_ra_ok;
}

/**
 * @brief Extract the binary payload after the second comma in @p capture.
 *
 * @details
 * Extracted from ::ra_da16600_tcp_recv. UM-WI-046 section 5.2.6
 * documents the ``+TRDTC:<cid>,<len>,<data>`` URC layout. The driver
 * here only needs to skip past two commas to land on the first byte
 * of the payload, then copy until newline or NUL bounded by @p cap.
 *
 * @param[in]  capture  Captured DA16600 response line.
 * @param[out] buf      Caller payload buffer.
 * @param[in]  cap      Capacity of @p buf in bytes.
 * @param[out] out_len  Bytes actually copied (0 on error).
 *
 * @return ::ra_err_t
 * @retval k_ra_ok            At least one payload byte was copied.
 * @retval k_ra_err_hw_timeout No ``:`` URC marker present in @p capture.
 * @retval k_ra_err_hw_error   URC was malformed (no second comma).
 *
 * @pre @p capture, @p buf, @p out_len are non-NULL.
 * @pre @p cap >= 1.
 * @post On any error path, @p *out_len == 0.
 * @post On success, @p *out_len <= @p cap.
 *
 * @note Not thread-safe; caller must serialise driver access.
 * @since 0.1.0
 */
static ra_err_t
internal_extract_trdtc_payload(const char* capture, uint8_t* buf, size_t cap, size_t* out_len)
{
  *out_len   = 0U;
  uint16_t i = 0U;
  while (capture[i] != '\0') {
    if (capture[i] == ':') {
      break;
    }
    ++i;
  }
  if (capture[i] != ':') {
    return k_ra_err_hw_timeout;
  }
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
  /* A carriage return is never present: ra_modem_at consumes every '\r' as a
   * line delimiter upstream, so a '\r' guard here would be dead code. */
  size_t oi = 0U;
  while ((capture[i] != '\0') && (capture[i] != '\n') && (oi < cap)) {
    buf[oi] = (uint8_t)capture[i];
    ++oi;
    ++i;
  }
  *out_len = oi;
  return k_ra_ok;
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

  if (role != k_ra_da16600_socket_listen) {
    if (role != k_ra_da16600_socket_connect) {
      ra_log_error(RA_DA16600_TAG, "invalid role");
      return k_ra_err_invalid_arg;
    }
  }
  if (role == k_ra_da16600_socket_connect) {
    RA_CHECK_NULL_PTR(remote_ip, RA_DA16600_TAG, "remote_ip");
  }

  ra_err_t err = ra_da16600_require_init();
  if (err != k_ra_ok) {
    return err;
  }

  char cmd[k_ra_da16600_cmd_buf_bytes];
  err = ra_da16600_build_tcp_open_cmd(cmd, sizeof cmd, role, remote_ip, port);
  if (err != k_ra_ok) {
    return err;
  }

  char capture[k_ra_da16600_capture_buf_bytes];
  capture[0] = '\0';
  err        = ra_modem_at_send_cmd_capture(cmd,
                                            capture,
                                            (size_t)k_ra_da16600_capture_buf_bytes,
                                            (uint16_t)k_ra_da16600_timeout_socket_ms);
  if (err != k_ra_ok) {
    return err;
  }
  return internal_parse_socket_cid(capture, out_socket);
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

  ra_err_t err = ra_da16600_require_init();
  if (err != k_ra_ok) {
    return err;
  }

  /* Build "AT+TRDTS=<cid>,<len>," prefix. The byte payload itself is
   * appended raw (length-prefixed framing -- UM-WI-046 5.2.5). */
  char   cmd[k_ra_da16600_cmd_buf_bytes];
  size_t off = 0U;
  err        = ra_da16600_build_trdts_header(cmd, sizeof cmd, sock, len, &off);
  if (err != k_ra_ok) {
    /* internal_build_trdts_header cannot fail: its overflow guards above are unreachable. */
    return err; /* GCOVR_EXCL_LINE */
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

  ra_err_t err = ra_da16600_require_init();
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
  return internal_extract_trdtc_payload(capture, buf, cap, out_len);
}

/** @brief Implementation of `ra_da16600_build_trtrm_cmd()` -- bounded append of
 *         the ``AT+TRTRM=<cid>`` terminate-session line. */
ra_err_t ra_da16600_build_trtrm_cmd(char* cmd, size_t cmd_cap, ra_da16600_socket_t sock)
{
  size_t off                           = 0U;
  cmd[0]                               = '\0';
  char num[k_ra_da16600_u32_str_bytes] = {};
  ra_da16600_format_u32(num, (uint32_t)sock);
  if ((ra_da16600_strcat_bounded(cmd, cmd_cap, &off, "AT+TRTRM=") == 0U) ||
      (ra_da16600_strcat_bounded(cmd, cmd_cap, &off, num) == 0U)) {
    ra_log_error(RA_DA16600_TAG, "TRTRM command overflow");
    return k_ra_err_invalid_size;
  }
  return k_ra_ok;
}

/* UM-WI-046 section 5.2.7 "Terminate Session": AT+TRTRM=<cid>. */
ra_err_t ra_da16600_tcp_close(ra_da16600_socket_t sock)
{
  ra_err_t err = ra_da16600_require_init();
  if (err != k_ra_ok) {
    return err;
  }
  char cmd[k_ra_da16600_cmd_buf_bytes];
  err = ra_da16600_build_trtrm_cmd(cmd, sizeof cmd, sock);
  if (err != k_ra_ok) {
    /* Unreachable in production: "AT+TRTRM=" + a <=3-digit cid is at most 12
     * bytes, far under the 96-byte buffer -- the overflow arms are exercised
     * for MC/DC directly against ra_da16600_build_trtrm_cmd with a tiny cap. */
    return err; /* GCOVR_EXCL_LINE */
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
  ra_err_t err = ra_da16600_require_init();
  if (err != k_ra_ok) {
    return err;
  }
  return ra_modem_at_send_cmd("AT+BLEADVSTART", nullptr, (uint16_t)k_ra_da16600_timeout_ble_ms);
}

/* UM-WI-046 section 7.4: AT+BLEADVSTOP. */
ra_err_t ra_da16600_ble_advertise_stop(void)
{
  ra_err_t err = ra_da16600_require_init();
  if (err != k_ra_ok) {
    return err;
  }
  return ra_modem_at_send_cmd("AT+BLEADVSTOP", nullptr, (uint16_t)k_ra_da16600_timeout_ble_ms);
}
