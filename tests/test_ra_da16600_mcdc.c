/**
 * @file test_ra_da16600_mcdc.c
 * @brief MC/DC supplement for ra_da16600.c compound boolean decisions
 *
 * @details
 * Companion to test_ra_da16600.c. The base suite covers the happy
 * paths and the simplest precondition rejections; this file drives the
 * still-uncovered arms of the compound decisions inside the DA16600 AT
 * driver -- command-buffer overflow OR-chains, response-parse loop
 * guards, and the multi-condition argument validators.
 *
 * The harness is copied verbatim from test_ra_da16600.c: a pair of
 * in-memory FIFOs stand in for the DA16600 UART, and a fake monotonic
 * clock lets timeouts be forced deterministically. Each test seeds the
 * modem-side RX FIFO with the exact bytes UM-WI-046 specifies for the
 * arm under test, runs the public API entry point, and asserts on the
 * returned ::ra_err_t (and any out-parameter) rather than on driver
 * internals.
 *
 * Coverage is achieved by the real driver function executing the
 * branch: every assertion targets an observable contract, never a
 * fragile private field.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_da16600.h"
#include "ra_da16600_internal.h"
#include "ra_err.h"
#include "unity_minimal.h"

/* ------------------------------------------------------------------------- */
/* Mock byte transport: two FIFOs + a fake monotonic clock. */
/* (Identical shape to test_ra_da16600.c so the two suites stay aligned.) */
/* ------------------------------------------------------------------------- */

/**
 * @enum da16600_mcdc_caps_t
 * @brief Sizing constants for the mock FIFO and the synthetic inputs.
 *
 * @details All values are typed C23 enums so the buffer-size sweep tool
 * can prove the test's local-frame budget without preprocessor
 * expansion, matching the production style rules.
 */
typedef enum : uint16_t {
  k_da16600_mcdc_fifo_cap     = 2048U, /**< Bytes in each mock FIFO.    */
  k_da16600_mcdc_line_buf_len = 512U,  /**< AT line accumulator length. */
} da16600_mcdc_caps_t;

/**
 * @enum da16600_mcdc_lengths_t
 * @brief Boundary lengths used to overflow the 96-byte command buffer.
 *
 * @details The DA16600 driver builds every AT line into a 96-byte
 * stack buffer (``k_ra_da16600_cmd_buf_bytes``). These lengths push the
 * concatenations past that ceiling to drive the overflow OR-chains.
 */
typedef enum : uint8_t {
  k_da16600_mcdc_ssid_overflow_len    = 31U, /**< Max SSID accepted by validate.    */
  k_da16600_mcdc_passkey_overflow_len = 63U, /**< Max passkey accepted by validate. */
  k_da16600_mcdc_ssid_too_long_len    = 32U, /**< First rejected SSID length.       */
  k_da16600_mcdc_passkey_too_long_len = 64U, /**< First rejected passkey length.    */
  k_da16600_mcdc_long_ip_len          = 90U, /**< Over-long IP to overflow TRTC.    */
} da16600_mcdc_lengths_t;

/**
 * @struct da16600_mcdc_fifo_t
 * @brief One direction of the mock UART.
 *
 * @details A flat byte buffer with separate head (read) and tail
 * (write) cursors; never wraps because the suite never enqueues more
 * than ::k_da16600_mcdc_fifo_cap bytes per test.
 *
 * @invariant ``head <= tail <= k_da16600_mcdc_fifo_cap``.
 */
typedef struct {
  uint8_t  buf[k_da16600_mcdc_fifo_cap]; /**< Backing storage.      */
  uint16_t head;                         /**< Next byte to pop.     */
  uint16_t tail;                         /**< Next free write slot. */
} da16600_mcdc_fifo_t;

/**
 * @struct da16600_mcdc_io_state_t
 * @brief Aggregate mock-transport state shared by the IO callbacks.
 *
 * @invariant ``fake_now_ms`` is monotonic non-decreasing.
 */
typedef struct {
  da16600_mcdc_fifo_t modem_to_mcu;    /**< Bytes the modem "sends" to us. */
  da16600_mcdc_fifo_t mcu_to_modem;    /**< Bytes the driver transmits.    */
  uint32_t            fake_now_ms;     /**< Current fake clock value.      */
  uint32_t            auto_advance_ms; /**< Added on every now_ms() read.  */
} da16600_mcdc_io_state_t;

/**
 * @var s_io
 * @brief Singleton mock-transport state.
 *
 * @warning Mutated by every test; reset via reset_world() before use.
 */
static da16600_mcdc_io_state_t s_io;

static void fifo_reset(da16600_mcdc_fifo_t* f)
{
  f->head = 0U;
  f->tail = 0U;
}

static void fifo_push(da16600_mcdc_fifo_t* f, uint8_t b)
{
  if (f->tail < (uint16_t)k_da16600_mcdc_fifo_cap) {
    f->buf[f->tail] = b;
    ++f->tail;
  }
}

static void fifo_push_str(da16600_mcdc_fifo_t* f, const char* s)
{
  uint16_t i = 0U;
  while (s[i] != '\0') {
    fifo_push(f, (uint8_t)s[i]);
    ++i;
  }
}

static int32_t fifo_pop(da16600_mcdc_fifo_t* f, uint8_t* out)
{
  if (f->head >= f->tail) {
    return -1;
  }
  *out = f->buf[f->head];
  ++f->head;
  return 0;
}

static ra_err_t mock_tx(void* ctx, uint8_t byte)
{
  (void)ctx;
  fifo_push(&s_io.mcu_to_modem, byte);
  return k_ra_ok;
}

static ra_err_t mock_rx(void* ctx, uint8_t* out)
{
  (void)ctx;
  if (fifo_pop(&s_io.modem_to_mcu, out) != 0) {
    return k_ra_err_no_data;
  }
  return k_ra_ok;
}

static uint32_t mock_now(void* ctx)
{
  (void)ctx;
  s_io.fake_now_ms += s_io.auto_advance_ms;
  return s_io.fake_now_ms;
}

static void reset_world(void)
{
  fifo_reset(&s_io.modem_to_mcu);
  fifo_reset(&s_io.mcu_to_modem);
  s_io.fake_now_ms     = 0U;
  s_io.auto_advance_ms = 0U;
}

/**
 * @var s_line_buf
 * @brief Caller-owned AT line accumulator handed to the driver.
 *
 * @warning Shared across tests; the driver overwrites it on every call.
 */
static uint8_t s_line_buf[k_da16600_mcdc_line_buf_len];

static ra_err_t bring_up_with_probe(void)
{
  reset_world();
  /* Pre-seed the bare-AT probe response so init succeeds. */
  fifo_push_str(&s_io.modem_to_mcu, "AT\r\n\r\nOK\r\n");
  ra_da16600_cfg_t cfg = {
    .io           = {.tx_byte = mock_tx, .rx_byte = mock_rx, .now_ms = mock_now, .ctx = nullptr},
    .line_buf     = s_line_buf,
    .line_buf_len = (uint16_t)sizeof s_line_buf,
    .default_timeout_ms = 1000U,
  };
  return ra_da16600_init(&cfg);
}

/**
 * @brief Fill @p dst with @p n copies of 'a', NUL-terminated.
 *
 * @details Builds the over-long SSID / passkey / IP strings used to
 * push the AT-command formatters past the 96-byte buffer ceiling. The
 * driver only inspects length and bytes, so a uniform fill is enough.
 *
 * @param[out] dst Destination buffer; must hold at least @p n + 1 bytes.
 * @param[in]  n   Number of 'a' characters to write.
 *
 * @pre @p dst is non-NULL with room for @p n + 1 bytes.
 * @pre @p n fits the caller's buffer.
 * @post @p dst[n] == '\0'.
 * @post Every byte in @p dst[0..n-1] is 'a'.
 *
 * @note Test-only helper; not thread-safe.
 */
static void fill_a(char* dst, uint16_t n)
{
  uint16_t i = 0U;
  while (i < n) {
    dst[i] = 'a';
    ++i;
  }
  dst[n] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Tests */
/* ------------------------------------------------------------------------- */

/**
 * @test test_wifi_connect_validate_ssid_too_long
 * @par MC/DC:
 * Decision `internal_wifi_connect_validate` L803:
 *   `(ssid_len == 0) || (ssid_len >= k_ssid_max) || (pk_len >= k_pk_max)`
 * - Base vector (happy, all false) lives in test_wifi_connect_happy.
 * - Empty-SSID vector (cond 1 true) lives in test_wifi_connect_empty_ssid.
 * - This vector: ssid_len = 32 -> cond 1 false, cond 2 true, cond 3
 *   false. Pairing this with the all-false base proves @c ssid_len's
 *   upper-bound term independently drives the decision to true.
 */
static void test_wifi_connect_validate_ssid_too_long(void)
{
  TEST_BEGIN("da16600 wifi_connect over-long SSID rejected");
  TEST_ASSERT_EQ(k_ra_ok, bring_up_with_probe());
  char ssid[k_da16600_mcdc_ssid_too_long_len + 1U];
  fill_a(ssid, (uint16_t)k_da16600_mcdc_ssid_too_long_len);
  char ip[k_ra_da16600_ip_str_len];
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_wifi_connect(ssid, "pwpwpwpw", ip, (size_t)k_ra_da16600_ip_str_len));
  TEST_END("da16600 wifi_connect over-long SSID rejected");
}

/**
 * @test test_wifi_connect_validate_passkey_too_long
 * @par MC/DC:
 * Decision `internal_wifi_connect_validate` L803 (3-condition OR):
 * - This vector: ssid_len = 8 -> cond 1 false, cond 2 false;
 *   pk_len = 64 -> cond 3 true. Pairing with the all-false base proves
 *   the passkey-length term independently drives the decision to true.
 */
static void test_wifi_connect_validate_passkey_too_long(void)
{
  TEST_BEGIN("da16600 wifi_connect over-long passkey rejected");
  TEST_ASSERT_EQ(k_ra_ok, bring_up_with_probe());
  char passkey[k_da16600_mcdc_passkey_too_long_len + 1U];
  fill_a(passkey, (uint16_t)k_da16600_mcdc_passkey_too_long_len);
  char ip[k_ra_da16600_ip_str_len];
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_wifi_connect("hil_lab", passkey, ip, (size_t)k_ra_da16600_ip_str_len));
  TEST_END("da16600 wifi_connect over-long passkey rejected");
}

/**
 * @test test_wifi_connect_cmd_overflow
 * @par MC/DC:
 * Decision `internal_build_wfjap_cmd` L330 (4-condition OR over the
 * ``internal_strcat_bounded() == 0`` overflow checks):
 * - Base vector (all four appends succeed -> decision false) lives in
 *   test_wifi_connect_happy.
 * - This vector: ssid = 31 'a', passkey = 63 'a'. The built line
 *   "AT+WFJAP="(9) + 31 + ",4,"(3) + 63 + NUL = 107 bytes overflows the
 *   96-byte command buffer, so the passkey append returns 0 and the OR
 *   short-circuits true. Validate still passes (31 < 32, 63 < 64), so
 *   the overflow is reached inside the formatter, not the validator.
 */
static void test_wifi_connect_cmd_overflow(void)
{
  TEST_BEGIN("da16600 wifi_connect WFJAP overflow rejected");
  TEST_ASSERT_EQ(k_ra_ok, bring_up_with_probe());
  char ssid[k_da16600_mcdc_ssid_overflow_len + 1U];
  char passkey[k_da16600_mcdc_passkey_overflow_len + 1U];
  fill_a(ssid, (uint16_t)k_da16600_mcdc_ssid_overflow_len);
  fill_a(passkey, (uint16_t)k_da16600_mcdc_passkey_overflow_len);
  char ip[k_ra_da16600_ip_str_len];
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_wifi_connect(ssid, passkey, ip, (size_t)k_ra_da16600_ip_str_len));
  TEST_END("da16600 wifi_connect WFJAP overflow rejected");
}

/**
 * @test test_wifi_connect_ip_stops_at_comma
 * @par MC/DC:
 * Decision `internal_parse_wfjap_ip` (4-condition AND copy guard):
 *   `(c != '\0') && (c != ',') && (c != '\n') && (oi+1 < ip_str_len)`
 * - This vector terminates the copy on the ``c != ','`` condition: the
 *   payload "+WFJAP:1,ssid,1.2.3.4,trailing" places a comma immediately
 *   after the dotted IP, so the loop copies "1.2.3.4" then sees ',' and
 *   stops with that one condition false. Paired with the happy-path
 *   vector (all four true while copying) this isolates the comma term.
 */
static void test_wifi_connect_ip_stops_at_comma(void)
{
  TEST_BEGIN("da16600 wifi_connect IP copy stops at comma");
  TEST_ASSERT_EQ(k_ra_ok, bring_up_with_probe());
  fifo_push_str(&s_io.modem_to_mcu,
                "AT+WFJAP=hil_lab,4,test1234\r\n"
                "+WFJAP:1,hil_lab,1.2.3.4,trailing\r\n"
                "\r\nOK\r\n");
  char ip[k_ra_da16600_ip_str_len];
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_da16600_wifi_connect("hil_lab", "test1234", ip, (size_t)k_ra_da16600_ip_str_len));
  TEST_ASSERT(strcmp(ip, "1.2.3.4") == 0);
  TEST_END("da16600 wifi_connect IP copy stops at comma");
}

/**
 * @test test_wifi_connect_ip_stops_at_newline
 * @par MC/DC:
 * Decision `internal_parse_wfjap_ip` (4-condition AND copy guard):
 * - This vector terminates the copy on the ``c != '\n'`` condition. The
 *   module emits two payload lines, so ra_modem_at joins them with a
 *   '\n' separator in the capture buffer. After copying "5.6.7.8" the
 *   loop meets that '\n' separator (the wire CR/LF were consumed as line
 *   delimiters) and stops, isolating the newline term. (A raw '\r' is
 *   never present -- ra_modem_at consumes it as a line delimiter -- so the
 *   copy guard no longer carries a dead '\r' term to isolate.)
 */
static void test_wifi_connect_ip_stops_at_newline(void)
{
  TEST_BEGIN("da16600 wifi_connect IP copy stops at newline");
  TEST_ASSERT_EQ(k_ra_ok, bring_up_with_probe());
  fifo_push_str(&s_io.modem_to_mcu,
                "AT+WFJAP=hil_lab,4,test1234\r\n"
                "+WFJAP:1,hil_lab,5.6.7.8\r\n"
                "+EXTRA:tail\r\n"
                "\r\nOK\r\n");
  char ip[k_ra_da16600_ip_str_len];
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_da16600_wifi_connect("hil_lab", "test1234", ip, (size_t)k_ra_da16600_ip_str_len));
  TEST_ASSERT(strcmp(ip, "5.6.7.8") == 0);
  TEST_END("da16600 wifi_connect IP copy stops at newline");
}

/**
 * @test test_wifi_connect_ip_fills_buffer
 * @par MC/DC:
 * Decision `internal_parse_wfjap_ip` (4-condition AND copy guard,
 * `(c != '\0') && (c != ',') && (c != '\n') && (oi+1 < ip_str_len)`):
 * - This vector terminates the copy on the ``oi + 1 < ip_str_len`` capacity
 *   condition with the three delimiter terms still true. The IP field is
 *   OVER-length ("255.255.255.25599", 17 chars) so the 16-byte buffer fills
 *   while the current byte is still a digit: at oi == ip_str_len-1 the bound
 *   goes false while ``c`` is a non-delimiter, isolating the bound term
 *   (`T,T,T,F`). A field that merely fits exactly would meet its delimiter
 *   first and short-circuit before the bound is evaluated.
 */
static void test_wifi_connect_ip_fills_buffer(void)
{
  TEST_BEGIN("da16600 wifi_connect IP copy stops at buffer bound");
  TEST_ASSERT_EQ(k_ra_ok, bring_up_with_probe());
  fifo_push_str(&s_io.modem_to_mcu,
                "AT+WFJAP=hil_lab,4,test1234\r\n"
                "+WFJAP:1,hil_lab,255.255.255.25599\r\n"
                "\r\nOK\r\n");
  char ip[k_ra_da16600_ip_str_len];
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_da16600_wifi_connect("hil_lab", "test1234", ip, (size_t)k_ra_da16600_ip_str_len));
  /* Copy truncates at ip_str_len-1 (15) bytes before the delimiter is met. */
  TEST_ASSERT(strcmp(ip, "255.255.255.255") == 0);
  TEST_END("da16600 wifi_connect IP copy stops at buffer bound");
}

/**
 * @test test_wifi_connect_ip_empty
 * @par MC/DC:
 * Decision `internal_parse_wfjap_ip` L389: `if (oi == 0)`.
 * - Base vector (oi > 0 after a non-empty IP) lives in the happy path.
 * - This vector drives oi == 0 true: the payload "+WFJAP:1,hil_lab,,more"
 *   has the second comma followed immediately by a third comma, so the
 *   two-comma scan (L370) exits with the cursor on a non-NUL byte and
 *   the L376 guard passes (commas == 2). The copy loop (L382) then sees
 *   that ',' on its very first read -- the ``c != ','`` term is false --
 *   so it runs zero iterations, leaving oi == 0 and tripping the empty-IP
 *   guard for a hw_error. This also exercises the L382 ``c != ','`` term
 *   firing on the first character.
 */
static void test_wifi_connect_ip_empty(void)
{
  TEST_BEGIN("da16600 wifi_connect empty IP rejected");
  TEST_ASSERT_EQ(k_ra_ok, bring_up_with_probe());
  fifo_push_str(&s_io.modem_to_mcu,
                "AT+WFJAP=hil_lab,4,test1234\r\n"
                "+WFJAP:1,hil_lab,,more\r\n"
                "\r\nOK\r\n");
  char ip[k_ra_da16600_ip_str_len];
  TEST_ASSERT_EQ(
    k_ra_err_hw_error,
    ra_da16600_wifi_connect("hil_lab", "test1234", ip, (size_t)k_ra_da16600_ip_str_len));
  TEST_ASSERT(ip[0] == '\0');
  TEST_END("da16600 wifi_connect empty IP rejected");
}

/**
 * @test test_tcp_open_connect_cmd_overflow
 * @par MC/DC:
 * Decision `internal_build_tcp_open_cmd` L503 (4-condition OR over the
 * TRTC ``internal_strcat_bounded() == 0`` checks):
 * - Base vector (all four appends succeed) lives in test_tcp_open_connect.
 * - This vector: a 90-byte remote_ip. "AT+TRTC="(8) + 90 already exceeds
 *   the 96-byte command buffer, so the remote_ip append returns 0 and
 *   the OR short-circuits true, yielding invalid_size before any command
 *   is transmitted.
 */
static void test_tcp_open_connect_cmd_overflow(void)
{
  TEST_BEGIN("da16600 tcp_open TRTC overflow rejected");
  TEST_ASSERT_EQ(k_ra_ok, bring_up_with_probe());
  char long_ip[k_da16600_mcdc_long_ip_len + 1U];
  fill_a(long_ip, (uint16_t)k_da16600_mcdc_long_ip_len);
  ra_da16600_socket_t sock = 0xFFU;
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_tcp_open(k_ra_da16600_socket_connect, long_ip, 80U, &sock));
  TEST_END("da16600 tcp_open TRTC overflow rejected");
}

/**
 * @test test_tcp_recv_no_colon
 * @par MC/DC:
 * Decision `internal_extract_trdtc_payload` L607: `if (c != ':')`.
 * - This vector drives the colon-scan loop to exhaust the string with no
 *   ':' present: the payload "nocolonhere" contains no URC marker, so the
 *   loop walks to '\0' and the post-loop guard finds ``c != ':''` true,
 *   returning hw_timeout. Out-length must be cleared to zero on error.
 */
static void test_tcp_recv_no_colon(void)
{
  TEST_BEGIN("da16600 tcp_recv no URC colon -> timeout");
  TEST_ASSERT_EQ(k_ra_ok, bring_up_with_probe());
  fifo_push_str(&s_io.modem_to_mcu, "AT\r\nnocolonhere\r\n\r\nOK\r\n");
  uint8_t buf[16];
  size_t  out_len = 7U;
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, ra_da16600_tcp_recv(0U, buf, sizeof buf, &out_len, 100U));
  TEST_ASSERT_EQ(0, out_len);
  TEST_END("da16600 tcp_recv no URC colon -> timeout");
}

/**
 * @test test_tcp_recv_missing_second_comma
 * @par MC/DC:
 * Decision `internal_extract_trdtc_payload` L611 (2-condition AND
 * comma-skip guard): `(c != '\0') && (commas < 2)` and the follow-on
 * L617 `if (c == '\0')`.
 * - Base vector (loop exits on ``commas < 2`` false after two commas)
 *   lives in test_tcp_recv_payload.
 * - This vector drives the loop to exit on the ``c != '\0'`` term: the
 *   payload "+TRDTC:5" has a colon but only zero commas, so the comma
 *   scan walks to '\0' with commas still < 2 and the L617 guard fires,
 *   returning hw_error. Pairing the two vectors isolates each AND term.
 */
static void test_tcp_recv_missing_second_comma(void)
{
  TEST_BEGIN("da16600 tcp_recv missing second comma -> hw_error");
  TEST_ASSERT_EQ(k_ra_ok, bring_up_with_probe());
  fifo_push_str(&s_io.modem_to_mcu, "AT\r\n+TRDTC:5\r\n\r\nOK\r\n");
  uint8_t buf[16];
  size_t  out_len = 9U;
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_da16600_tcp_recv(0U, buf, sizeof buf, &out_len, 100U));
  TEST_ASSERT_EQ(0, out_len);
  TEST_END("da16600 tcp_recv missing second comma -> hw_error");
}

/**
 * @test test_tcp_recv_truncates_to_cap
 * @par MC/DC:
 * Decision `internal_extract_trdtc_payload` L621 (4-condition AND
 * payload-copy guard):
 *   `(c != '\0') && (c != '\r') && (c != '\n') && (oi < cap)`
 * - This vector drives the ``oi < cap`` term false: a 5-byte payload
 *   "hello" is copied into a 2-byte buffer, so after two bytes the
 *   capacity term goes false while the three delimiter terms are still
 *   true. The copy stops at the cap and reports out_len == 2.
 */
static void test_tcp_recv_truncates_to_cap(void)
{
  TEST_BEGIN("da16600 tcp_recv truncates to cap");
  TEST_ASSERT_EQ(k_ra_ok, bring_up_with_probe());
  fifo_push_str(&s_io.modem_to_mcu, "AT\r\n+TRDTC:0,5,hello\r\n\r\nOK\r\n");
  uint8_t buf[2];
  size_t  out_len = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_da16600_tcp_recv(0U, buf, sizeof buf, &out_len, 100U));
  TEST_ASSERT_EQ(2, out_len);
  TEST_ASSERT_EQ('h', buf[0]);
  TEST_ASSERT_EQ('e', buf[1]);
  TEST_END("da16600 tcp_recv truncates to cap");
}

/**
 * @test test_tcp_recv_payload_stops_at_newline
 * @par MC/DC:
 * Decision `internal_extract_trdtc_payload` L621 (4-condition AND
 * payload-copy guard):
 * - This vector drives the ``c != '\n'`` term false. Two payload lines
 *   make ra_modem_at join them with a '\n' separator in the capture, so
 *   after copying "ab" the loop meets that '\n' and stops, isolating the
 *   newline term. (The bare-'\r' term is not independently reachable:
 *   raw CR is consumed as a line delimiter and never reaches the copy
 *   loop -- see the unreachable note in the report.)
 */
static void test_tcp_recv_payload_stops_at_newline(void)
{
  TEST_BEGIN("da16600 tcp_recv payload stops at newline");
  TEST_ASSERT_EQ(k_ra_ok, bring_up_with_probe());
  fifo_push_str(&s_io.modem_to_mcu, "AT\r\n+TRDTC:0,2,ab\r\nTAILLINE\r\n\r\nOK\r\n");
  uint8_t buf[16];
  size_t  out_len = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_da16600_tcp_recv(0U, buf, sizeof buf, &out_len, 100U));
  TEST_ASSERT_EQ(2, out_len);
  TEST_ASSERT_EQ('a', buf[0]);
  TEST_ASSERT_EQ('b', buf[1]);
  TEST_END("da16600 tcp_recv payload stops at newline");
}

/**
 * @test test_build_wfjap_cmd_overflow_mcdc
 * @par MC/DC:
 * Decision `ra_da16600_build_wfjap_cmd` (4-condition OR over the bounded
 * ``ra_da16600_strcat_bounded(...) == 0`` overflow checks). The production
 * 96-byte buffer never overflows on the length-validated ssid/passkey, so the
 * prefix / ssid / separator arms are reached by calling the promoted builder
 * directly with a cmd_cap sized so exactly one append fails:
 * - V1: cap=96 (full) -> F,F,F,F -> k_ra_ok (all appends fit).
 * - V2: cap=5  < len("AT+WFJAP=")=9 -> T,.,.,. (prefix append fails).
 * - V3: cap=10 fits the 9-byte prefix (+NUL) only -> F,T,.,. (ssid fails).
 * - V4: cap=13 fits prefix+"net" only -> F,F,T,. (",4," append fails).
 * - V5: cap=16 fits prefix+"net"+",4," only -> F,F,F,T (passkey append fails).
 * N+1 = 5 vectors for N=4 conditions: minimal MC/DC.
 */
static void test_build_wfjap_cmd_overflow_mcdc(void)
{
  TEST_BEGIN("da16600 build_wfjap_cmd overflow OR (MC/DC)");
  char cmd[k_ra_da16600_cmd_buf_bytes];
  TEST_ASSERT_EQ(k_ra_ok, ra_da16600_build_wfjap_cmd(cmd, sizeof cmd, "net", "pass")); /* FFFF */
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_da16600_build_wfjap_cmd(cmd, 5U, "net", "pass")); /* T */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_build_wfjap_cmd(cmd, 10U, "net", "pass")); /* FT */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_build_wfjap_cmd(cmd, 13U, "net", "pass")); /* FFT */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_build_wfjap_cmd(cmd, 16U, "net", "pass")); /* FFFT */
  TEST_END("da16600 build_wfjap_cmd overflow OR (MC/DC)");
}

int32_t main(void)
{
  test_wifi_connect_validate_ssid_too_long();
  test_wifi_connect_validate_passkey_too_long();
  test_wifi_connect_cmd_overflow();
  test_build_wfjap_cmd_overflow_mcdc();

  test_wifi_connect_ip_stops_at_comma();
  test_wifi_connect_ip_stops_at_newline();
  test_wifi_connect_ip_fills_buffer();
  test_wifi_connect_ip_empty();

  test_tcp_open_connect_cmd_overflow();

  test_tcp_recv_no_colon();
  test_tcp_recv_missing_second_comma();
  test_tcp_recv_truncates_to_cap();
  test_tcp_recv_payload_stops_at_newline();

  (void)fprintf(stderr, "[OK ] test_ra_da16600_mcdc.c\n");
  return 0;
}
