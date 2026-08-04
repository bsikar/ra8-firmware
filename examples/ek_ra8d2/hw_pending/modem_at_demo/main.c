/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_pending/modem_at_demo/main.c
 * @brief Cellular AT-modem bring-up + query demo over the MikroBUS UART (SCI7)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Stand-alone EK-RA8D2 application that drives ``libs/ra8_modem_at`` against a
 * 3GPP AT-command cellular modem (SIM7600 / Quectel BG95 class) presented on
 * the MikroBUS UART -- RXD7 / TXD7, i.e. SCI channel 7
 * (``k_ra8_board_mikrobus_uart_sci_channel``). A MikroE cellular Click
 * (LTE IoT / 4G LTE / NB-IoT) exposes its modem on exactly these MikroBUS pads.
 *
 * TODO(real modem): the physical RF link + a populated MikroBUS cellular Click
 * with a live SIM are external hardware this repo cannot carry, so the app is
 * hw_pending. ra8_emulator answers the AT script faithfully with the SCI7 modem
 * model (``tools/ra8_emulator/src/periph/board_periph_modem.c``, ``--modem``), so the app
 * boots and walks its whole state machine 0-skip in EIL (EIL == HIL for the AT
 * protocol; only the antenna is unmodelled).
 *
 * State machine (each step is one blocking ``ra8_modem_at`` command):
 *   1. sync         : ``AT`` / ``ATE0`` / ``AT+CMEE=1`` (probe, no-echo, numeric
 *                     CME errors).
 *   2. SIM          : ``AT+CPIN?`` -> expect ``+CPIN: READY``.
 *   3. signal       : ``AT+CSQ``   -> parse RSSI (0..31, 99 = unknown).
 *   4. registration : ``AT+CREG=1`` then poll the unsolicited ``+CREG`` URC,
 *                     then ``AT+CREG?`` -> registration status (1 home / 5 roam).
 *   5. attach       : ``AT+CGATT?`` -> PS attach flag.
 *   6. error path   : an unsupported command -> the modem replies
 *                     ``+CME ERROR: 4``; the driver surfaces
 *                     ``k_ra8_err_hw_error`` and the app treats that as the
 *                     EXPECTED outcome for this step.
 *
 * On success the app prints (SCI8 console, scraped by ra8_emulator / a HIL rig):
 *
 *     modem: rssi=17 reg=1 attach=1 cme=ok PASS
 *
 * On any unexpected step failure it prints ``modem: <step> FAIL``, latches LED2
 * and parks. The compound decisions (registration OK, signal valid, per-step
 * expected outcome, overall verdict) are host-tested with MC/DC in
 * ``tests/test_app_modem_at_demo.c``.
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_modem_at.h"
#include "ra8_mstp.h"
#include "ra8_port_utils.h"
#include "ra8_sci.h"
#include "ra8_time.h"

/* =============================================================================
 * App-wide tunables
 * =============================================================================
 */

/** @brief App-wide numeric tunables (typed enum -- no magic numbers). */
typedef enum : uint32_t {
  k_modem_baud         = 115200U, /**< SIM7600/BG95 default line rate.   */
  k_modem_console_baud = 115200U, /**< J-Link OB VCOM console line rate. */
  k_modem_heartbeat_ms = 500U,    /**< LED heartbeat period after PASS.  */
  k_modem_dec_base     = 10U,     /**< Radix for integer-to-ASCII.       */
  /** SCI7. */
  k_modem_uart_channel = (uint32_t)k_ra8_board_mikrobus_uart_sci_channel,
} modem_const_t;

/** @brief Per-command timeout handed to ``ra8_modem_at`` (ms). */
typedef enum : uint16_t {
  k_modem_cmd_timeout_ms = 2000U, /**< Modem cmd timeout ms. */
} modem_timeout_t;

/** @brief Small fixed buffer capacities. */
typedef enum : uint32_t {
  k_modem_dec_cap     = 10U, /**< Digits for a uint32_t decimal (+ slack). */
  k_modem_capture_cap = 64U, /**< Capture buffer for one response payload. */
  k_modem_scan_max    = 64U, /**< Static bound for every line-scan loop.   */
} modem_cap_t;

/** @brief CSQ (signal-quality) sentinels: RSSI range 0..31, 99 = unknown. */
typedef enum : uint8_t {
  k_modem_csq_min_bars = 5U,  /**< Minimum RSSI treated as a usable signal. */
  k_modem_csq_max      = 31U, /**< Highest valid CSQ RSSI index.            */
  k_modem_csq_unknown  = 99U, /**< CSQ "not known / not detectable".        */
} modem_csq_t;

/** @brief +CREG registration-status codes (3GPP 27.007). */
typedef enum : uint8_t {
  k_modem_creg_not_registered = 0U, /**< Not registered, not searching.  */
  k_modem_creg_home           = 1U, /**< Registered, home network.       */
  k_modem_creg_searching      = 2U, /**< Not registered, searching.      */
  k_modem_creg_denied         = 3U, /**< Registration denied.            */
  k_modem_creg_unknown        = 4U, /**< Unknown (e.g. out of coverage). */
  k_modem_creg_roaming        = 5U, /**< Registered, roaming.            */
} modem_creg_stat_t;

/** @brief CGATT PS-attach flag values. */
typedef enum : uint8_t {
  k_modem_cgatt_detached = 0U, /**< Not attached to the PS domain. */
  k_modem_cgatt_attached = 1U, /**< Attached to the PS domain.     */
} modem_cgatt_t;

/* =============================================================================
 * Module state (file scope -- NASA P10 Rule 6)
 * =============================================================================
 */

/**
 * @var s_creg_stat
 * @brief Latest registration status parsed from a ``+CREG`` line.
 * @note Written by ::modem_on_creg_urc (URC + solicited); read for the verdict.
 * @warning Direct external modification is forbidden.
 * @since 0.1.0
 */
static uint8_t s_creg_stat;

/**
 * @var s_creg_seen
 * @brief True once at least one ``+CREG`` line has been parsed.
 * @note Written by ::modem_on_creg_urc.
 * @warning Direct external modification is forbidden.
 * @since 0.1.0
 */
static bool s_creg_seen;

/* Banner strings scraped by ra8_emulator / the HIL rig. The success line is
 * "modem: rssi=17 reg=1 attach=1 cme=ok PASS"; a failed verdict swaps PASS for
 * FAIL and reflects the actual cme= token. */
static const uint8_t k_modem_banner_prefix[] = "modem: rssi=";
static const uint8_t k_modem_str_reg[]       = " reg=";
static const uint8_t k_modem_str_attach[]    = " attach=";
static const uint8_t k_modem_str_cme[]       = " cme=";
static const uint8_t k_modem_str_ok[]        = "ok";
static const uint8_t k_modem_str_no[]        = "no";
static const uint8_t k_modem_str_pass[]      = " PASS\r\n";
static const uint8_t k_modem_str_fail[]      = " FAIL\r\n";

/* =============================================================================
 * Pure decision logic (mirrored + MC/DC-tested in tests/test_app_modem_at_demo.c)
 * =============================================================================
 */

/**
 * @brief Is the modem registered on a network (home or roaming)?
 *
 * @param[in] stat A ``+CREG`` status code (::modem_creg_stat_t value).
 * @return true iff @p stat is "registered, home" or "registered, roaming".
 * @retval true  @p stat == 1 or @p stat == 5.
 * @retval false Any other status (not registered / searching / denied / unknown).
 *
 * @pre @p stat is a 3GPP 27.007 +CREG status code.
 * @pre No shared state is read.
 * @post No state is mutated.
 * @post Return value depends solely on @p stat.
 *
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
static bool modem_creg_registered(uint32_t stat)
{
  return (stat == (uint32_t)k_modem_creg_home) || (stat == (uint32_t)k_modem_creg_roaming);
}

/**
 * @brief Is a CSQ RSSI reading a known, usable signal?
 *
 * @details
 * Two independent conditions: the reading must be a real measurement (not the
 * 99 "unknown" sentinel) AND at least ::k_modem_csq_min_bars of strength. The
 * two are genuinely independent -- 99 (unknown) is above the strength floor, so
 * either can fail alone -- which is what makes the compound MC/DC-testable.
 *
 * @param[in] rssi A CSQ RSSI index parsed from ``+CSQ: <rssi>,<ber>``.
 * @return true iff @p rssi is a real reading with usable strength.
 * @retval true  @p rssi != 99 and @p rssi >= ::k_modem_csq_min_bars.
 * @retval false @p rssi == 99 (unknown) or @p rssi below the usable floor.
 *
 * @pre @p rssi is the first field of a +CSQ response.
 * @pre No shared state is read.
 * @post No state is mutated.
 * @post Return value depends solely on @p rssi.
 *
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
static bool modem_signal_valid(uint32_t rssi)
{
  return (rssi != (uint32_t)k_modem_csq_unknown) && (rssi >= (uint32_t)k_modem_csq_min_bars);
}

/**
 * @brief Did an AT step return the outcome the app expected?
 *
 * @details
 * Every step but the deliberate error probe expects ``k_ra8_ok``. The error
 * probe sends an unsupported command and EXPECTS the modem's ``+CME ERROR``
 * (which the driver surfaces as ``k_ra8_err_hw_error``): for that step a plain
 * ``OK`` would actually be a failure. This inverts the pass condition on the
 * probe step, which is the interesting compound decision.
 *
 * @param[in] is_cme_probe true for the intentional unsupported-command step.
 * @param[in] err          The ``ra8_modem_at`` return code for that step.
 * @return true iff @p err is the expected outcome for the step.
 * @retval true  (probe and @p err == hw_error) or (non-probe and @p err == ok).
 * @retval false Otherwise.
 *
 * @pre @p err came from a completed ``ra8_modem_at_send_cmd*`` call.
 * @pre No shared state is read.
 * @post No state is mutated.
 * @post Return value depends solely on @p is_cme_probe and @p err.
 *
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
static bool modem_step_passed(bool is_cme_probe, ra8_err_t err)
{
  return (is_cme_probe && (err == k_ra8_err_hw_error)) || ((!is_cme_probe) && (err == k_ra8_ok));
}

/**
 * @brief Overall run verdict: every probed condition must hold.
 *
 * @param[in] sim_ready  SIM reported ``+CPIN: READY``.
 * @param[in] signal_ok  CSQ RSSI was a valid reading.
 * @param[in] registered +CREG status was home or roaming.
 * @param[in] attached   CGATT reported attached.
 * @param[in] cme_ok      The error-probe step returned the expected CME error.
 * @return true iff ALL five inputs are true.
 * @retval true  Every condition held.
 * @retval false Any one condition failed.
 *
 * @pre The five flags were derived from a completed AT run.
 * @pre No shared state is read.
 * @post No state is mutated.
 * @post Return value depends solely on the five inputs.
 *
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
static bool
modem_run_ok(bool sim_ready, bool signal_ok, bool registered, bool attached, bool cme_ok)
{
  return sim_ready && signal_ok && registered && attached && cme_ok;
}

/* =============================================================================
 * Pure parsing helpers (also mirrored + tested on the host)
 * =============================================================================
 */

/**
 * @brief Parse the first unsigned integer after the ``:`` in an AT reply line.
 *
 * @details Used for ``+CSQ: 17,99`` (-> 17) and ``+CGATT: 1`` (-> 1). Every
 * scan loop is bounded by ::k_modem_scan_max (NASA P10 Rule 2).
 *
 * @param[in]  line NUL-terminated reply line (may be NULL -> failure).
 * @param[out] out  Parsed value on success (may be NULL -> failure).
 * @return true iff a colon-delimited unsigned integer was parsed.
 * @retval true  @p out holds the first integer after the colon.
 * @retval false No colon, no digit, or a NULL argument.
 *
 * @pre @p line is NUL-terminated within ::k_modem_scan_max bytes.
 * @pre @p out points to writable storage when non-NULL.
 * @post @p out is written only on success.
 * @post No other state is mutated.
 *
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
static bool modem_parse_first_uint(const char* line, uint32_t* out)
{
  if ((line == nullptr) || (out == nullptr)) {
    return false;
  }
  uint32_t i = 0U;
  while ((i < (uint32_t)k_modem_scan_max) && (line[i] != '\0') && (line[i] != ':')) {
    i++;
  }
  if ((i >= (uint32_t)k_modem_scan_max) || (line[i] != ':')) {
    return false;
  }
  i++;
  while ((i < (uint32_t)k_modem_scan_max) && (line[i] == ' ')) {
    i++;
  }
  if ((i >= (uint32_t)k_modem_scan_max) || (line[i] < '0') || (line[i] > '9')) {
    return false;
  }
  uint32_t v = 0U;
  while ((i < (uint32_t)k_modem_scan_max) && (line[i] >= '0') && (line[i] <= '9')) {
    v = (v * (uint32_t)k_modem_dec_base) + (uint32_t)(line[i] - '0');
    i++;
  }
  *out = v;
  return true;
}

/**
 * @brief Parse the LAST unsigned integer anywhere in an AT reply line.
 *
 * @details The +CREG status is the last field in both forms the modem sends:
 * the URC ``+CREG: <stat>`` and the solicited ``+CREG: <n>,<stat>``. Scanning
 * for the last digit run yields @c stat for both. Bounded by ::k_modem_scan_max.
 *
 * @param[in]  line NUL-terminated reply line (may be NULL -> failure).
 * @param[out] out  Parsed value on success (may be NULL -> failure).
 * @return true iff at least one digit run was found.
 * @retval true  @p out holds the value of the last digit run.
 * @retval false No digit found, or a NULL argument.
 *
 * @pre @p line is NUL-terminated within ::k_modem_scan_max bytes.
 * @pre @p out points to writable storage when non-NULL.
 * @post @p out is written only on success.
 * @post No other state is mutated.
 *
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
static bool modem_parse_last_uint(const char* line, uint32_t* out)
{
  if ((line == nullptr) || (out == nullptr)) {
    return false;
  }
  bool     found = false;
  uint32_t v     = 0U;
  uint32_t i     = 0U;
  while ((i < (uint32_t)k_modem_scan_max) && (line[i] != '\0')) {
    if ((line[i] >= '0') && (line[i] <= '9')) {
      v = 0U;
      while ((i < (uint32_t)k_modem_scan_max) && (line[i] >= '0') && (line[i] <= '9')) {
        v = (v * (uint32_t)k_modem_dec_base) + (uint32_t)(line[i] - '0');
        i++;
      }
      found = true;
    } else {
      i++;
    }
  }
  if (found) {
    *out = v;
  }
  return found;
}

/**
 * @brief Does @p line contain the substring @p needle?
 *
 * @param[in] line   NUL-terminated line to search (may be NULL -> false).
 * @param[in] needle NUL-terminated substring (may be NULL -> false).
 * @return true iff @p needle occurs within @p line.
 * @retval true  A match was found.
 * @retval false No match, or a NULL argument.
 *
 * @pre Both strings are NUL-terminated within ::k_modem_scan_max bytes.
 * @pre No shared state is read.
 * @post No state is mutated.
 *
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
static bool modem_line_contains(const char* line, const char* needle)
{
  if ((line == nullptr) || (needle == nullptr)) {
    return false;
  }
  for (uint32_t i = 0U; (i < (uint32_t)k_modem_scan_max) && (line[i] != '\0'); i++) {
    uint32_t j = 0U;
    while ((j < (uint32_t)k_modem_scan_max) && (needle[j] != '\0') && (line[i + j] == needle[j])) {
      j++;
    }
    if (needle[j] == '\0') {
      return true;
    }
  }
  return false;
}

/* =============================================================================
 * Small integer serialiser + console emit helpers
 * =============================================================================
 */

/**
 * @brief Convert an unsigned 32-bit int into a decimal ASCII string.
 *
 * @param[in]  value Integer to convert.
 * @param[out] out   Destination buffer (>= ::k_modem_dec_cap bytes).
 * @return Number of bytes written (excluding any NUL).
 *
 * @pre @p out is non-NULL and >= ::k_modem_dec_cap bytes.
 * @pre The result never exceeds ::k_modem_dec_cap digits.
 * @post @p out holds the decimal digits, most-significant first.
 * @post The returned length is in ``[1, k_modem_dec_cap]``.
 *
 * @note Not thread-safe with a shared @p out buffer.
 * @since 0.1.0
 */
static uint32_t modem_u32_to_dec(uint32_t value, uint8_t* out)
{
  uint8_t  tmp[k_modem_dec_cap] = {};
  uint32_t n                    = 0U;
  if (value == 0U) {
    tmp[n] = (uint8_t)'0';
    n++;
  } else {
    while ((value > 0U) && (n < (uint32_t)k_modem_dec_cap)) {
      tmp[n] = (uint8_t)('0' + (uint8_t)(value % (uint32_t)k_modem_dec_base));
      n++;
      value = value / (uint32_t)k_modem_dec_base;
    }
  }
  for (uint32_t i = 0U; i < n; i++) {
    out[i] = tmp[n - 1U - i];
  }
  return n;
}

/**
 * @brief Emit a byte run on the J-Link OB VCOM console (SCI8).
 *
 * @param[in] bytes Byte array to transmit (non-NULL when @p len > 0).
 * @param[in] len   Number of bytes.
 *
 * @pre @p bytes is non-NULL when @p len > 0.
 * @pre The console was initialised by ::modem_setup_or_halt.
 * @post All @p len bytes have been handed to SCI8.
 * @post No app state is mutated.
 *
 * @since 0.1.0
 */
static void modem_tx(const uint8_t* bytes, uint32_t len)
{
  (void)ra8_board_uart_console_write(bytes, (size_t)len);
}

/**
 * @brief Emit a labelled unsigned integer: ``<label><value>``.
 *
 * @param[in] label     NUL-terminated label bytes.
 * @param[in] label_len Length of @p label in bytes.
 * @param[in] value     Value to print in decimal.
 *
 * @pre @p label is non-NULL.
 * @pre The console was initialised.
 * @post ``<label><value>`` has been transmitted on SCI8.
 * @post No app state is mutated.
 *
 * @since 0.1.0
 */
static void modem_emit_kv(const uint8_t* label, uint32_t label_len, uint32_t value)
{
  uint8_t buf[k_modem_dec_cap] = {};
  modem_tx(label, label_len);
  const uint32_t n = modem_u32_to_dec(value, buf);
  modem_tx(buf, n);
}

/* =============================================================================
 * ra8_modem_at transport adapters (SCI7, polled)
 * =============================================================================
 */

/* cppcheck-suppress constParameterCallback ; ctx must match the ra8_modem_at
 * io.tx_byte function-pointer typedef (void*). */
static ra8_err_t modem_tx_byte(void* ctx, uint8_t byte)
{
  (void)ctx;
  return ra8_sci_putc_polling((uint8_t)k_modem_uart_channel, byte);
}

/* cppcheck-suppress constParameterCallback ; ctx must match the ra8_modem_at
 * io.rx_byte function-pointer typedef (void*). A bounded getc timeout maps to
 * "no byte", which the driver treats as "keep waiting". */
static ra8_err_t modem_rx_byte(void* ctx, uint8_t* out_byte)
{
  (void)ctx;
  return ra8_sci_getc_polling((uint8_t)k_modem_uart_channel, out_byte);
}

/* cppcheck-suppress constParameterCallback ; ctx must match the ra8_modem_at
 * io.now_ms function-pointer typedef (void*). */
static uint32_t modem_now_ms(void* ctx)
{
  (void)ctx;
  return ra8_now_ms();
}

/**
 * @brief URC handler for unsolicited + solicited ``+CREG`` lines.
 *
 * @details Parses the registration status (last integer field) and latches it
 * for the run verdict. Registered with ``ra8_modem_at_register_unsolicited_handler``
 * so both the ``AT+CREG=1`` enable URC and the ``AT+CREG?`` reply route here.
 *
 * @param[in] line NUL-terminated ``+CREG ...`` line (non-NULL from the driver).
 * @param[in] ctx  Unused opaque context.
 *
 * @pre @p line is a NUL-terminated modem line beginning with ``+CREG:``.
 * @pre The module ::s_creg_stat / ::s_creg_seen are single-owner.
 * @post On a parseable status, ::s_creg_stat / ::s_creg_seen are updated.
 * @post No transmit occurs from within the handler.
 *
 * @note Not thread-safe; the demo is single-threaded.
 * @since 0.1.0
 */
static void modem_on_creg_urc(const char* line, void* ctx)
{
  (void)ctx;
  if (line == nullptr) {
    return;
  }
  uint32_t stat = 0U;
  if (modem_parse_last_uint(line, &stat)) {
    s_creg_stat = (uint8_t)stat;
    s_creg_seen = true;
  }
}

/* =============================================================================
 * Bring-up helpers (kept out of main() for NASA P10 Rule 4)
 * =============================================================================
 */

/**
 * @brief Park forever after a fatal error.
 *
 * @pre Called only after a fatal init/step failure.
 * @post The CPU is parked; only a debugger or reset wakes it.
 * @since 0.1.0
 */
static void modem_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + MSTP up; publish PCLKA via @p out_pclka_hz.
 *
 * @param[out] out_pclka_hz Receives the live PCLKA frequency on success.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre @p out_pclka_hz is non-NULL.
 * @post On success ``*out_pclka_hz`` is non-zero and CGC/MSTP/SysTick are live.
 * @post On any failure the function never returns (parks).
 *
 * @since 0.1.0
 */
static void modem_clocks_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    modem_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    modem_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, out_pclka_hz) != k_ra8_ok) {
    modem_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    modem_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    modem_panic_halt();
  }
}

/**
 * @brief Route the MikroBUS UART pins (TXD7 / RXD7) via PFS.
 *
 * @pre ``ra8_mstp_init`` already ran so PFS writes land.
 * @post Both SCI7 UART pins are in their peripheral PSEL on success.
 * @post On any failure the function never returns (parks).
 *
 * @since 0.1.0
 */
static void modem_pfs_or_halt(void)
{
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) -- RA8_PIN()-packed board pin is valid ra8_port_pin_t data outside the enumerator list. */
  if (ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_mikrobus_tx,
                               k_ra8_psel_sci_async,
                               "modem_at_demo.txd7") != k_ra8_ok) {
    modem_panic_halt();
  }
  if (ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_mikrobus_rx,
                               k_ra8_psel_sci_async,
                               "modem_at_demo.rxd7") != k_ra8_ok) {
    modem_panic_halt();
  }
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
}

/**
 * @brief Bring CGC + console (SCI8) + modem UART (SCI7) + LEDs up.
 *
 * @pre Reset copied .data and zeroed .bss.
 * @post SCI8 console + SCI7 modem UART are live; LED1/LED2 are configured.
 * @post On any failure the function never returns (parks).
 *
 * @since 0.1.0
 */
static void modem_setup_or_halt(void)
{
  uint32_t pclka_hz = 0U;
  modem_clocks_or_halt(&pclka_hz);
  modem_pfs_or_halt();

  if (ra8_board_uart_console_init((uint32_t)k_modem_console_baud) != k_ra8_ok) {
    modem_panic_halt();
  }
  const ra8_sci_cfg_t cfg = {
    .baud      = (uint32_t)k_modem_baud,
    .data_bits = k_ra8_sci_data_8,
    .parity    = k_ra8_sci_parity_none,
    .stop_bits = k_ra8_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra8_sci_init((uint8_t)k_modem_uart_channel, &cfg) != k_ra8_ok) {
    modem_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    modem_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    modem_panic_halt();
  }
}

/**
 * @brief Bind ``ra8_modem_at`` to the SCI7 transport + register the URC handler.
 *
 * @param[in] line_buf     Caller-owned line accumulator for the driver.
 * @param[in] line_buf_len Capacity of @p line_buf.
 *
 * @return ``ra8_err_t`` from init / handler registration.
 * @retval k_ra8_ok Bound and the ``+CREG`` handler is installed.
 * @retval other    The first non-OK code from init / registration.
 *
 * @pre @p line_buf is non-NULL and @p line_buf_len >= 16.
 * @pre SCI7 is initialised.
 * @post On success the driver is ready and the URC handler is live.
 * @post On failure the driver is left uninitialised.
 *
 * @since 0.1.0
 */
static ra8_err_t modem_bind(uint8_t* line_buf, uint16_t line_buf_len)
{
  const ra8_modem_at_cfg_t cfg = {
    .io                 = {.tx_byte = modem_tx_byte,
                           .rx_byte = modem_rx_byte,
                           .now_ms  = modem_now_ms,
                           .ctx     = nullptr},
    .line_buf           = line_buf,
    .line_buf_len       = line_buf_len,
    .default_timeout_ms = (uint16_t)k_modem_cmd_timeout_ms,
  };
  const ra8_err_t ierr = ra8_modem_at_init(&cfg);
  if (ierr != k_ra8_ok) {
    return ierr;
  }
  return ra8_modem_at_register_unsolicited_handler("+CREG:", modem_on_creg_urc, nullptr);
}

/**
 * @brief Send an AT command and require a bare ``OK``.
 *
 * @param[in] cmd NUL-terminated AT command (no trailing CR).
 * @return The ``ra8_modem_at_send_cmd`` return code.
 *
 * @pre The driver was bound by ::modem_bind.
 * @pre @p cmd is NUL-terminated.
 * @post The command has been transmitted and its result awaited.
 * @post No app state is mutated here.
 *
 * @since 0.1.0
 */
static ra8_err_t modem_cmd_ok(const char* cmd)
{
  return ra8_modem_at_send_cmd(cmd, nullptr, (uint16_t)k_modem_cmd_timeout_ms);
}

/**
 * @brief Send an AT query and capture its response payload.
 *
 * @param[in]  cmd NUL-terminated AT command (no trailing CR).
 * @param[out] buf Capture buffer for the payload lines.
 * @param[in]  cap Capacity of @p buf.
 * @return The ``ra8_modem_at_send_cmd_capture`` return code.
 *
 * @pre The driver was bound by ::modem_bind.
 * @pre @p buf is non-NULL and @p cap >= 1.
 * @post On success @p buf is NUL-terminated with the captured payload.
 * @post No app state is mutated here.
 *
 * @since 0.1.0
 */
static ra8_err_t modem_query(const char* cmd, char* buf, size_t cap)
{
  return ra8_modem_at_send_cmd_capture(cmd, buf, cap, (uint16_t)k_modem_cmd_timeout_ms);
}

/**
 * @brief Print a ``modem: <step> FAIL`` banner, latch LED2, and park.
 *
 * @param[in] step     NUL-terminated failing-step label.
 * @param[in] step_len Length of @p step in bytes.
 *
 * @pre The console is initialised.
 * @post The FAIL banner is emitted, LED2 is ON, and the CPU parks.
 * @post The function never returns.
 *
 * @since 0.1.0
 */
static void modem_fail(const uint8_t* step, uint32_t step_len)
{
  static const uint8_t k_pfx[] = "modem: ";
  static const uint8_t k_sfx[] = " FAIL\r\n";
  modem_tx(k_pfx, (uint32_t)(sizeof(k_pfx) - 1U));
  modem_tx(step, step_len);
  modem_tx(k_sfx, (uint32_t)(sizeof(k_sfx) - 1U));
  (void)ra8_board_led_on(k_ra8_board_led2);
  modem_panic_halt();
}

/* =============================================================================
 * AT script phases
 * =============================================================================
 */

/**
 * @brief Sync phase: probe (``AT``), disable echo (``ATE0``), numeric CME errors.
 *
 * @details Any non-OK here parks via ::modem_fail (the modem is absent / dead).
 *
 * @pre The driver was bound.
 * @post On return all three sync commands returned OK.
 * @post On any failure the function never returns.
 *
 * @since 0.1.0
 */
static void modem_phase_sync(void)
{
  static const uint8_t k_step[] = "sync";
  if (modem_cmd_ok("AT") != k_ra8_ok) {
    modem_fail(k_step, (uint32_t)(sizeof(k_step) - 1U));
  }
  if (modem_cmd_ok("ATE0") != k_ra8_ok) {
    modem_fail(k_step, (uint32_t)(sizeof(k_step) - 1U));
  }
  if (modem_cmd_ok("AT+CMEE=1") != k_ra8_ok) {
    modem_fail(k_step, (uint32_t)(sizeof(k_step) - 1U));
  }
}

/**
 * @brief SIM phase: query ``AT+CPIN?`` and confirm ``READY``.
 *
 * @return true iff the SIM reported ``+CPIN: READY``.
 * @retval true  SIM is ready.
 * @retval false SIM not ready (query error path parks via ::modem_fail).
 *
 * @pre The driver was bound.
 * @post On query failure the function never returns.
 * @post On success the return reflects the ``READY`` substring test.
 *
 * @since 0.1.0
 */
static bool modem_phase_sim(void)
{
  static const uint8_t k_step[]                 = "cpin";
  char                 buf[k_modem_capture_cap] = {};
  if (modem_query("AT+CPIN?", buf, sizeof buf) != k_ra8_ok) {
    modem_fail(k_step, (uint32_t)(sizeof(k_step) - 1U));
  }
  return modem_line_contains(buf, "READY");
}

/**
 * @brief Signal phase: query ``AT+CSQ`` and parse + print the RSSI.
 *
 * @param[out] out_rssi Receives the parsed RSSI (or 99 if unparsed).
 * @return true iff the RSSI is a valid reading (::modem_signal_valid).
 *
 * @pre The driver was bound and @p out_rssi is non-NULL.
 * @post On query failure the function never returns.
 * @post ``*out_rssi`` holds the parsed RSSI; the banner prefix is emitted.
 *
 * @since 0.1.0
 */
static bool modem_phase_signal(uint32_t* out_rssi)
{
  static const uint8_t k_step[]                 = "csq";
  char                 buf[k_modem_capture_cap] = {};
  if (modem_query("AT+CSQ", buf, sizeof buf) != k_ra8_ok) {
    modem_fail(k_step, (uint32_t)(sizeof(k_step) - 1U));
  }
  uint32_t rssi = (uint32_t)k_modem_csq_unknown;
  (void)modem_parse_first_uint(buf, &rssi);
  *out_rssi = rssi;
  modem_emit_kv(k_modem_banner_prefix, (uint32_t)(sizeof(k_modem_banner_prefix) - 1U), rssi);
  return modem_signal_valid(rssi);
}

/**
 * @brief Registration phase: enable ``+CREG`` URCs, poll one, then query.
 *
 * @return true iff the latched registration status is home or roaming.
 *
 * @pre The driver was bound with the ``+CREG`` URC handler installed.
 * @post On any command failure the function never returns.
 * @post ::s_creg_stat reflects the latest ``+CREG`` line; ``reg=`` is emitted.
 *
 * @since 0.1.0
 */
static bool modem_phase_registration(void)
{
  static const uint8_t k_step[] = "creg";
  if (modem_cmd_ok("AT+CREG=1") != k_ra8_ok) {
    modem_fail(k_step, (uint32_t)(sizeof(k_step) - 1U));
  }
  /* Drain + dispatch the unsolicited "+CREG: <stat>" the modem emits on enable. */
  (void)ra8_modem_at_poll();
  if (modem_cmd_ok("AT+CREG?") != k_ra8_ok) {
    modem_fail(k_step, (uint32_t)(sizeof(k_step) - 1U));
  }
  modem_emit_kv(k_modem_str_reg, (uint32_t)(sizeof(k_modem_str_reg) - 1U), (uint32_t)s_creg_stat);
  return s_creg_seen && modem_creg_registered((uint32_t)s_creg_stat);
}

/**
 * @brief Attach phase: query ``AT+CGATT?`` and parse + print the flag.
 *
 * @return true iff the modem reported PS-attached (flag == 1).
 *
 * @pre The driver was bound.
 * @post On query failure the function never returns.
 * @post ``attach=`` is emitted with the parsed flag.
 *
 * @since 0.1.0
 */
static bool modem_phase_attach(void)
{
  static const uint8_t k_step[]                 = "cgatt";
  char                 buf[k_modem_capture_cap] = {};
  if (modem_query("AT+CGATT?", buf, sizeof buf) != k_ra8_ok) {
    modem_fail(k_step, (uint32_t)(sizeof(k_step) - 1U));
  }
  uint32_t flag = (uint32_t)k_modem_cgatt_detached;
  (void)modem_parse_first_uint(buf, &flag);
  modem_emit_kv(k_modem_str_attach, (uint32_t)(sizeof(k_modem_str_attach) - 1U), flag);
  return flag == (uint32_t)k_modem_cgatt_attached;
}

/**
 * @brief Error-path phase: send an unsupported command, expect ``+CME ERROR``.
 *
 * @return true iff the driver surfaced the expected ``k_ra8_err_hw_error``.
 *
 * @pre The driver was bound and ``AT+CMEE=1`` enabled numeric CME errors.
 * @post No console output here (the verdict banner reports ``cme=``).
 * @post No app state is mutated.
 *
 * @since 0.1.0
 */
static bool modem_phase_error(void)
{
  const ra8_err_t err = modem_cmd_ok("AT+NOSUCHCMD");
  return modem_step_passed(true, err);
}

/* =============================================================================
 * Main
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: bring up SCI7 + the AT driver, walk the state machine.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On a clean run the PASS banner is printed and the CPU heartbeats forever.
 * @post On any failure LED2 latches ON and the CPU parks.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  modem_setup_or_halt();
  ra8_isr_globals_enable();

  static uint8_t s_line[k_modem_capture_cap * 2U];
  if (modem_bind(s_line, (uint16_t)sizeof s_line) != k_ra8_ok) {
    static const uint8_t k_step[] = "init";
    modem_fail(k_step, (uint32_t)(sizeof(k_step) - 1U));
  }

  modem_phase_sync();
  const bool sim_ready  = modem_phase_sim();
  uint32_t   rssi       = (uint32_t)k_modem_csq_unknown;
  const bool signal_ok  = modem_phase_signal(&rssi);
  const bool registered = modem_phase_registration();
  const bool attached   = modem_phase_attach();
  const bool cme_ok     = modem_phase_error();

  const bool ok = modem_run_ok(sim_ready, signal_ok, registered, attached, cme_ok);
  /* "ok"/"no" are both 2 bytes and " PASS\r\n"/" FAIL\r\n" both 7, so one length
   * expression per pair keeps -Wduplicated-branches happy. */
  modem_tx(k_modem_str_cme, (uint32_t)(sizeof(k_modem_str_cme) - 1U));
  modem_tx(cme_ok ? k_modem_str_ok : k_modem_str_no, (uint32_t)(sizeof(k_modem_str_ok) - 1U));
  modem_tx(ok ? k_modem_str_pass : k_modem_str_fail, (uint32_t)(sizeof(k_modem_str_pass) - 1U));

  while (1) {
    (void)ra8_board_led_toggle(k_ra8_board_led1);
    ra8_delay_ms((uint32_t)k_modem_heartbeat_ms);
  }

  modem_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
