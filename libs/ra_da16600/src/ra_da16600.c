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
#include "ra_da16600_internal.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_modem_at.h"

/* =============================================================================
 * Module-static state
 * =============================================================================
 */

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

/** @brief Implementation of `ra_da16600_strcat_bounded()` -- offset-cursor append. */
uint8_t ra_da16600_strcat_bounded(char* dst, size_t cap, size_t* off, const char* src)
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

/** @brief Implementation of `ra_da16600_format_u32()` -- reverse-digit decimal. */
void ra_da16600_format_u32(char* dst, uint32_t value)
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

/** @brief Implementation of `ra_da16600_require_init()` -- reads the s_da gate. */
ra_err_t ra_da16600_require_init(void)
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
  if ((ra_da16600_strcat_bounded(cmd, cmd_cap, &off, "AT+WFJAP=") == 0U) ||
      (ra_da16600_strcat_bounded(cmd, cmd_cap, &off, ssid) == 0U) ||
      (ra_da16600_strcat_bounded(cmd, cmd_cap, &off, ",4,") == 0U) ||
      (ra_da16600_strcat_bounded(cmd, cmd_cap, &off, passkey) == 0U)) {
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
  ra_err_t err = ra_da16600_require_init();
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

  err = ra_da16600_require_init();
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
  ra_err_t err = ra_da16600_require_init();
  if (err != k_ra_ok) {
    return err;
  }
  return ra_modem_at_send_cmd("AT+WFQAP", nullptr, (uint16_t)k_ra_da16600_timeout_probe_ms);
}
