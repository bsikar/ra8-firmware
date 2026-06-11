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
  k_ra_da16600_cmd_buf_bytes     = 96U,   /**< Per-command AT line buffer (TX). */
  k_ra_da16600_capture_buf_bytes = 256U,  /**< Per-command response capture. */
  k_ra_da16600_payload_max_bytes = 1460U, /**< One TCP MSS. UM-WI-046 5.2.5. */
} ra_da16600_internal_caps_t;

/**
 * @brief Stack-buffer sizing for decimal uint32_t formatting.
 *
 * @details
 * The widest decimal representation of a @c uint32_t is the literal
 * @c "4294967295" -- ten characters. Adding one trailing NUL puts the
 * total at eleven bytes. These two constants are typed enums so the
 * compiler picks a stable 8-bit width and the values appear by name in
 * the disassembly / debugger, satisfying NASA P10 Rule 8 and the
 * project-wide ``readability-magic-numbers`` lint gate.
 */
typedef enum : uint8_t {
  k_ra_da16600_u32_digit_max = 10U, /**< Max decimal digits in a uint32_t. */
  k_ra_da16600_u32_str_bytes = 11U, /**< Digits + NUL terminator. */
  k_ra_da16600_decimal_base  = 10U, /**< Decimal radix for the formatter. */
} ra_da16600_format_caps_t;

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
 * @details Walks @p s until the first NUL byte or until @c UINT16_MAX is
 * reached, whichever comes first. The cap prevents a runaway loop on a
 * non-terminated input.
 *
 * @param[in] s NUL-terminated string (must be non-NULL).
 * @return Length in bytes.
 * @retval 0..UINT16_MAX Number of bytes before the first NUL.
 *
 * @pre @p s is non-NULL.
 * @pre @p s is NUL-terminated before @c UINT16_MAX.
 * @post No state mutated.
 * @post Return value <= @c UINT16_MAX.
 *
 * @note Pure function; safe to call concurrently.
 * @since 0.1.0
 */
static uint16_t internal_str_len(const char* s)
{
  uint16_t i = 0U;
  /* cppcheck-suppress arrayIndexOutOfBoundsCond
   * Reason: this is a NUL-bounded scan; the upper bound exists only as
   * a safety cap for unterminated input. cppcheck can't tell the caller
   * always passes a NUL-terminated string, so it flags the conditional
   * as "redundant or OOB". */
  while (i < (uint16_t)UINT16_MAX) {
    /* cppcheck-suppress arrayIndexOutOfBoundsCond */
    if (s[i] == '\0') {
      break;
    }
    ++i;
  }
  return i;
}

/**
 * @brief Append @p src to @p dst, NUL-terminate, bounded by @p cap.
 *
 * @details Strict-bounded variant of @c strcat that uses an explicit
 * offset cursor instead of re-scanning @p dst on every call. Used by
 * the AT-command formatters in this TU to build a multi-piece command
 * line without ever pulling in @c <string.h>.
 *
 * @param[in,out] dst Destination buffer (NUL-terminated on exit).
 * @param[in]     cap Capacity of @p dst including NUL.
 * @param[in,out] off In: current write offset. Out: updated offset.
 * @param[in]     src NUL-terminated string to append.
 *
 * @return Status flag.
 * @retval 1 Appended successfully; @p *off advanced past @p src.
 * @retval 0 Appending would overflow; @p dst forcibly NUL-terminated.
 *
 * @pre @p dst, @p off, @p src non-NULL.
 * @pre @p cap >= 1.
 * @post On success, ``dst[*off] == '\0'``.
 * @post On overflow, ``dst[cap-1] == '\0'`` and result is 0.
 *
 * @note Not thread-safe; caller must serialise driver access.
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
 * target build. Writes at most ::k_ra_da16600_u32_str_bytes bytes
 * ("4294967295" + NUL).
 *
 * The intermediate @c tmp buffer is zero-initialised before use so the
 * clang static analyser can prove every later read of the reversed
 * digits is well-defined regardless of the @p value path taken; this
 * also matches NASA P10 Rule 5's "no garbage on egress" guarantee.
 *
 * @param[out] dst    Destination; must hold at least
 *                    ::k_ra_da16600_u32_str_bytes bytes.
 * @param[in]  value  Value to format.
 *
 * @pre @p dst is non-NULL.
 * @pre @p dst points to at least ::k_ra_da16600_u32_str_bytes bytes
 *      of writable storage.
 * @post @c dst is NUL-terminated.
 * @post Every byte written is a printable ASCII digit ('0'..'9').
 *
 * @note Pure function; safe to call concurrently.
 * @since 0.1.0
 */
static void internal_format_u32(char* dst, uint32_t value)
{
  /* Worst case "4294967295" = k_ra_da16600_u32_digit_max digits. */
  char     tmp[k_ra_da16600_u32_str_bytes] = {};
  uint8_t  ti                              = 0U;
  uint32_t v                               = value;
  if (v == 0U) {
    dst[0] = '0';
    dst[1] = '\0';
    return;
  }
  while ((v > 0U) && (ti < (uint8_t)k_ra_da16600_u32_digit_max)) {
    tmp[ti] = (char)('0' + (uint8_t)(v % (uint32_t)k_ra_da16600_decimal_base));
    v /= (uint32_t)k_ra_da16600_decimal_base;
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
 * @param[in] s NUL-terminated input string (may be NULL).
 * @return Entry count (0 if empty / NULL).
 * @retval 0 @p s is NULL or empty.
 * @retval >0 One plus the number of separators in @p s.
 *
 * @pre @p s may be NULL; this is part of the contract.
 * @pre When non-NULL, @p s is NUL-terminated.
 * @post No state mutated.
 * @post Return value fits in @c uint16_t.
 *
 * @note Pure function; safe to call concurrently.
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
    if (s[i] == '/') {
      ++count;
    }
    if (s[i] == '\x01') {
      ++count;
    }
    ++i;
  }
  return count;
}

/**
 * @brief Verify driver was initialized before any command call.
 *
 * @details Gate used by every public command entry point to reject
 * calls that arrived before ::ra_da16600_init returned ::k_ra_ok.
 * Keeps the per-function pre-condition logic concentrated in one
 * place.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok                  Driver has been initialised.
 * @retval k_ra_err_not_initialized ::ra_da16600_init has not yet run.
 *
 * @pre @c s_da has been written by ::ra_da16600_init or zero-initialised.
 * @pre Caller is on the single owner thread (driver is single-instance).
 * @post No state mutated.
 * @post Return value is exactly one of the documented retvals.
 *
 * @note Not thread-safe; caller must serialise driver access.
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

/**
 * @brief Build the ``AT+WFJAP=<ssid>,4,<key>`` command into @p cmd.
 *
 * @details
 * Splits out the command-formatting step of ::ra_da16600_wifi_connect
 * to keep the public entry point inside the NASA P10 Rule 4 60-line
 * ceiling and the @c readability-function-size threshold. Security
 * mode 4 ("WPA2-PSK") is hard-coded per UM-WI-046 Table 4.5-1.
 *
 * @param[out] cmd       Caller-owned command buffer.
 * @param[in]  cmd_cap   Capacity of @p cmd in bytes (incl. NUL).
 * @param[in]  ssid      NUL-terminated SSID.
 * @param[in]  passkey   NUL-terminated WPA2 passphrase.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok              Command formatted into @p cmd.
 * @retval k_ra_err_invalid_size One of the concatenations overflowed.
 *
 * @pre @p cmd, @p ssid, @p passkey are non-NULL.
 * @pre @p cmd_cap > 0.
 * @post On success, @p cmd is a NUL-terminated AT command line.
 * @post On overflow, @p cmd is NUL-terminated at @c cmd_cap-1.
 *
 * @note Not thread-safe; caller must serialise driver access.
 * @since 0.1.0
 */
static ra_err_t
internal_build_wfjap_cmd(char* cmd, size_t cmd_cap, const char* ssid, const char* passkey)
{
  size_t off = 0U;
  cmd[0]     = '\0';
  if ((internal_strcat_bounded(cmd, cmd_cap, &off, "AT+WFJAP=") == 0U) ||
      (internal_strcat_bounded(cmd, cmd_cap, &off, ssid) == 0U) ||
      (internal_strcat_bounded(cmd, cmd_cap, &off, ",4,") == 0U) ||
      (internal_strcat_bounded(cmd, cmd_cap, &off, passkey) == 0U)) {
    ra_log_error(RA_DA16600_TAG, "WFJAP command overflows cmd buffer");
    return k_ra_err_invalid_size;
  }
  return k_ra_ok;
}

/**
 * @brief Parse ``+WFJAP:1,<ssid>,<ip>`` and copy the IP string out.
 *
 * @details
 * UM-WI-046 section 4.5 documents the URC layout: the IPv4 address
 * sits after the second comma. This helper walks the captured line,
 * skips two commas, and copies the dotted-decimal IPv4 address into
 * @p out_ip_str up to the next delimiter (comma, CR, LF, NUL).
 *
 * @param[in]  capture     Captured DA16600 response line.
 * @param[out] out_ip_str  Caller buffer for dotted-decimal IPv4.
 * @param[in]  ip_str_len  Capacity of @p out_ip_str.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok           IP successfully parsed and copied.
 * @retval k_ra_err_hw_error Capture line missing two commas or empty IP.
 *
 * @pre @p capture and @p out_ip_str are non-NULL.
 * @pre @p ip_str_len >= ::k_ra_da16600_ip_str_len.
 * @post On success, @p out_ip_str is a non-empty NUL-terminated string.
 * @post On error, @p out_ip_str[0] is NUL.
 *
 * @note Not thread-safe; caller must serialise driver access.
 * @since 0.1.0
 */
static ra_err_t internal_parse_wfjap_ip(const char* capture, char* out_ip_str, size_t ip_str_len)
{
  out_ip_str[0]   = '\0';
  uint16_t i      = 0U;
  uint16_t commas = 0U;
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
  size_t oi = 0U;
  while ((capture[i] != '\0') && (capture[i] != ',') && (capture[i] != '\r') &&
         (capture[i] != '\n') && (oi + 1U < ip_str_len)) {
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

/**
 * @brief Build the ``AT+TRTS=<port>`` or ``AT+TRTC=<ip>,<port>`` line.
 *
 * @details
 * Extracted from ::ra_da16600_tcp_open so the public entry point fits
 * inside the cognitive-complexity ceiling. UM-WI-046 sections 5.2.3
 * (listen) and 5.2.4 (connect) define the two command shapes.
 *
 * @param[out] cmd        Caller-owned command buffer.
 * @param[in]  cmd_cap    Capacity of @p cmd in bytes.
 * @param[in]  role       Listen vs connect role.
 * @param[in]  remote_ip  Dotted-decimal IPv4 (connect role only).
 * @param[in]  port       TCP port in host byte order.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok              Command formatted into @p cmd.
 * @retval k_ra_err_invalid_size A concatenation overflowed @p cmd_cap.
 *
 * @pre @p cmd is non-NULL and @p cmd_cap >= 1.
 * @pre When @p role is connect, @p remote_ip is non-NULL.
 * @post On success, @p cmd is a NUL-terminated AT command line.
 * @post On overflow, @p cmd is NUL-terminated at @c cmd_cap-1.
 *
 * @note Not thread-safe; caller must serialise driver access.
 * @since 0.1.0
 */
static ra_err_t internal_build_tcp_open_cmd(char*                    cmd,
                                            size_t                   cmd_cap,
                                            ra_da16600_socket_role_t role,
                                            const char*              remote_ip,
                                            uint16_t                 port)
{
  size_t off                                = 0U;
  cmd[0]                                    = '\0';
  char port_str[k_ra_da16600_u32_str_bytes] = {};
  internal_format_u32(port_str, (uint32_t)port);

  if (role == k_ra_da16600_socket_listen) {
    if ((internal_strcat_bounded(cmd, cmd_cap, &off, "AT+TRTS=") == 0U) ||
        (internal_strcat_bounded(cmd, cmd_cap, &off, port_str) == 0U)) {
      ra_log_error(RA_DA16600_TAG, "TRTS command overflow");
      return k_ra_err_invalid_size;
    }
    return k_ra_ok;
  }
  if ((internal_strcat_bounded(cmd, cmd_cap, &off, "AT+TRTC=") == 0U) ||
      (internal_strcat_bounded(cmd, cmd_cap, &off, remote_ip) == 0U) ||
      (internal_strcat_bounded(cmd, cmd_cap, &off, ",") == 0U) ||
      (internal_strcat_bounded(cmd, cmd_cap, &off, port_str) == 0U)) {
    ra_log_error(RA_DA16600_TAG, "TRTC command overflow");
    return k_ra_err_invalid_size;
  }
  return k_ra_ok;
}

/**
 * @brief Build the ``AT+TRDTS=<cid>,<len>,`` header (no payload).
 *
 * @details
 * Extracted from ::ra_da16600_tcp_send so the public entry point fits
 * inside the NASA P10 Rule 4 ceiling. UM-WI-046 section 5.2.5 defines
 * the header layout; the raw binary payload is appended by the caller
 * after this helper returns. @p out_off is the new write offset so
 * the caller knows where to start copying its payload bytes.
 *
 * @param[out] cmd      Caller-owned command buffer.
 * @param[in]  cmd_cap  Capacity of @p cmd in bytes.
 * @param[in]  sock     TCP socket / cid.
 * @param[in]  len      Payload length in bytes.
 * @param[out] out_off  Final write offset into @p cmd.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok              Header formatted, @p *out_off updated.
 * @retval k_ra_err_invalid_size A concatenation overflowed @p cmd_cap.
 *
 * @pre @p cmd and @p out_off are non-NULL.
 * @pre @p cmd_cap >= 1.
 * @post On success, @p cmd[*out_off] is NUL and the header is complete.
 * @post On overflow, @p cmd is NUL-terminated and @p *out_off is 0.
 *
 * @note Not thread-safe; caller must serialise driver access.
 * @since 0.1.0
 */
static ra_err_t internal_build_trdts_header(char*               cmd,
                                            size_t              cmd_cap,
                                            ra_da16600_socket_t sock,
                                            size_t              len,
                                            size_t*             out_off)
{
  size_t off = 0U;
  cmd[0]     = '\0';
  *out_off   = 0U;

  char num[k_ra_da16600_u32_str_bytes] = {};
  internal_format_u32(num, (uint32_t)sock);
  if ((internal_strcat_bounded(cmd, cmd_cap, &off, "AT+TRDTS=") == 0U) ||
      (internal_strcat_bounded(cmd, cmd_cap, &off, num) == 0U) ||
      (internal_strcat_bounded(cmd, cmd_cap, &off, ",") == 0U)) {
    ra_log_error(RA_DA16600_TAG, "TRDTS command overflow");
    return k_ra_err_invalid_size;
  }
  internal_format_u32(num, (uint32_t)len);
  if ((internal_strcat_bounded(cmd, cmd_cap, &off, num) == 0U) ||
      (internal_strcat_bounded(cmd, cmd_cap, &off, ",") == 0U)) {
    ra_log_error(RA_DA16600_TAG, "TRDTS command overflow");
    return k_ra_err_invalid_size;
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
  size_t oi = 0U;
  while ((capture[i] != '\0') && (capture[i] != '\r') && (capture[i] != '\n') && (oi < cap)) {
    buf[oi] = (uint8_t)capture[i];
    ++oi;
    ++i;
  }
  *out_len = oi;
  return k_ra_ok;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Validate that every IO callback in @p io is populated.
 *
 * @details Split from ::ra_da16600_init so the init body stays under
 * the function-size gate; each callback gets its own check (no
 * compound conditions, per the MC/DC policy).
 *
 * @param[in] io Byte-transport descriptor to validate.
 * @return ::ra_err_t
 * @retval k_ra_ok           All three callbacks are non-NULL.
 * @retval k_ra_err_null_ptr At least one callback is NULL.
 *
 * @pre @p io points to a readable descriptor.
 * @pre None beyond the pointer contract.
 * @post No state is modified.
 * @post The verdict reflects all three callback slots.
 * @note Pure check; logs on failure.
 * @since 0.1.0
 */
static ra_err_t internal_validate_io(const ra_modem_at_io_t* io)
{
  if (io->tx_byte == nullptr) {
    ra_log_error(RA_DA16600_TAG, "io callbacks must be non-NULL");
    return k_ra_err_null_ptr;
  }
  if (io->rx_byte == nullptr) {
    ra_log_error(RA_DA16600_TAG, "io callbacks must be non-NULL");
    return k_ra_err_null_ptr;
  }
  if (io->now_ms == nullptr) {
    ra_log_error(RA_DA16600_TAG, "io callbacks must be non-NULL");
    return k_ra_err_null_ptr;
  }
  return k_ra_ok;
}

/* UM-WI-046 section 2.1 "Basic AT Commands": bare ``AT`` returns OK. */
ra_err_t ra_da16600_init(const ra_da16600_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, RA_DA16600_TAG, "cfg");
  RA_CHECK_NULL_PTR(cfg->line_buf, RA_DA16600_TAG, "cfg->line_buf");
  if (cfg->line_buf_len < (uint16_t)k_ra_da16600_min_line_buf) {
    ra_log_error(RA_DA16600_TAG, "line_buf_len below floor");
    return k_ra_err_invalid_size;
  }
  const ra_err_t io_err = internal_validate_io(&cfg->io);
  if (io_err != k_ra_ok) {
    return io_err;
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
  err        = ra_modem_at_send_cmd_capture("AT+WFSCAN",
                                            capture,
                                            (size_t)k_ra_da16600_capture_buf_bytes,
                                            (uint16_t)k_ra_da16600_timeout_scan_ms);
  if (err != k_ra_ok) {
    return err;
  }

  /* Locate the "+WFSCAN:" prefix and count entries after it. */
  uint16_t i = 0U;
  while (capture[i] != '\0') {
    if (capture[i] == ':') {
      break;
    }
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

/**
 * @brief Validate the argument tuple passed to ::ra_da16600_wifi_connect.
 *
 * @details
 * Splits the precondition checks (NULL pointers, output-buffer size
 * floor, SSID / passkey length window) out of the public entry point
 * so that function fits inside the NASA P10 Rule 4 statement budget.
 *
 * @param[in] ssid       NUL-terminated SSID.
 * @param[in] passkey    NUL-terminated WPA2 passphrase.
 * @param[in] out_ip_str Caller buffer for dotted-decimal IPv4.
 * @param[in] ip_str_len Capacity of @p out_ip_str.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok              All preconditions hold.
 * @retval k_ra_err_null_ptr    One of @p ssid, @p passkey, @p out_ip_str
 *                              is NULL.
 * @retval k_ra_err_invalid_size SSID empty/too long, passkey too long,
 *                              or @p ip_str_len below the floor.
 *
 * @pre All three pointer arguments may legally be NULL; that is what
 *      this helper checks.
 * @pre @c k_ra_da16600_ssid_max_len and @c k_ra_da16600_passkey_max_len
 *      reflect UM-WI-046 section 4.5 limits.
 * @post On error, @p out_ip_str (if non-NULL) is left untouched.
 * @post On success, all four arguments are valid for ::ra_da16600_wifi_connect.
 *
 * @note Not thread-safe; caller must serialise driver access.
 * @since 0.1.0
 */
static ra_err_t internal_wifi_connect_validate(const char* ssid,
                                               const char* passkey,
                                               const char* out_ip_str,
                                               size_t      ip_str_len)
{
  RA_CHECK_NULL_PTR(ssid, RA_DA16600_TAG, "ssid");
  RA_CHECK_NULL_PTR(passkey, RA_DA16600_TAG, "passkey");
  RA_CHECK_NULL_PTR(out_ip_str, RA_DA16600_TAG, "out_ip_str");
  if (ip_str_len < (size_t)k_ra_da16600_ip_str_len) {
    ra_log_error(RA_DA16600_TAG, "ip_str_len too small");
    return k_ra_err_invalid_size;
  }
  uint16_t ssid_len = internal_str_len(ssid);
  uint16_t pk_len   = internal_str_len(passkey);
  if ((ssid_len == 0U) || (ssid_len >= (uint16_t)k_ra_da16600_ssid_max_len) ||
      (pk_len >= (uint16_t)k_ra_da16600_passkey_max_len)) {
    ra_log_error(RA_DA16600_TAG, "ssid/passkey length invalid");
    return k_ra_err_invalid_size;
  }
  return k_ra_ok;
}

/* UM-WI-046 section 4.5 "Connect / Disconnect AP": AT+WFJAP. */
ra_err_t
ra_da16600_wifi_connect(const char* ssid, const char* passkey, char* out_ip_str, size_t ip_str_len)
{
  ra_err_t err = internal_wifi_connect_validate(ssid, passkey, out_ip_str, ip_str_len);
  if (err != k_ra_ok) {
    return err;
  }
  out_ip_str[0] = '\0';

  err = internal_require_init();
  if (err != k_ra_ok) {
    return err;
  }

  char cmd[k_ra_da16600_cmd_buf_bytes];
  err = internal_build_wfjap_cmd(cmd, sizeof cmd, ssid, passkey);
  if (err != k_ra_ok) {
    return err;
  }

  char capture[k_ra_da16600_capture_buf_bytes];
  capture[0] = '\0';
  err        = ra_modem_at_send_cmd_capture(cmd,
                                            capture,
                                            (size_t)k_ra_da16600_capture_buf_bytes,
                                            (uint16_t)k_ra_da16600_timeout_connect_ms);
  if (err != k_ra_ok) {
    return err;
  }
  return internal_parse_wfjap_ip(capture, out_ip_str, ip_str_len);
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

  if (role != k_ra_da16600_socket_listen) {
    if (role != k_ra_da16600_socket_connect) {
      ra_log_error(RA_DA16600_TAG, "invalid role");
      return k_ra_err_invalid_arg;
    }
  }
  if (role == k_ra_da16600_socket_connect) {
    RA_CHECK_NULL_PTR(remote_ip, RA_DA16600_TAG, "remote_ip");
  }

  ra_err_t err = internal_require_init();
  if (err != k_ra_ok) {
    return err;
  }

  char cmd[k_ra_da16600_cmd_buf_bytes];
  err = internal_build_tcp_open_cmd(cmd, sizeof cmd, role, remote_ip, port);
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

  ra_err_t err = internal_require_init();
  if (err != k_ra_ok) {
    return err;
  }

  /* Build "AT+TRDTS=<cid>,<len>," prefix. The byte payload itself is
   * appended raw (length-prefixed framing -- UM-WI-046 5.2.5). */
  char   cmd[k_ra_da16600_cmd_buf_bytes];
  size_t off = 0U;
  err        = internal_build_trdts_header(cmd, sizeof cmd, sock, len, &off);
  if (err != k_ra_ok) {
    return err;
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
  return internal_extract_trdtc_payload(capture, buf, cap, out_len);
}

/* UM-WI-046 section 5.2.7 "Terminate Session": AT+TRTRM=<cid>. */
ra_err_t ra_da16600_tcp_close(ra_da16600_socket_t sock)
{
  ra_err_t err = internal_require_init();
  if (err != k_ra_ok) {
    return err;
  }
  char   cmd[k_ra_da16600_cmd_buf_bytes];
  size_t off                           = 0U;
  cmd[0]                               = '\0';
  char num[k_ra_da16600_u32_str_bytes] = {};
  internal_format_u32(num, (uint32_t)sock);
  if ((internal_strcat_bounded(cmd, sizeof cmd, &off, "AT+TRTRM=") == 0U) ||
      (internal_strcat_bounded(cmd, sizeof cmd, &off, num) == 0U)) {
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
  return ra_modem_at_send_cmd("AT+BLEADVSTART", nullptr, (uint16_t)k_ra_da16600_timeout_ble_ms);
}

/* UM-WI-046 section 7.4: AT+BLEADVSTOP. */
ra_err_t ra_da16600_ble_advertise_stop(void)
{
  ra_err_t err = internal_require_init();
  if (err != k_ra_ok) {
    return err;
  }
  return ra_modem_at_send_cmd("AT+BLEADVSTOP", nullptr, (uint16_t)k_ra_da16600_timeout_ble_ms);
}
