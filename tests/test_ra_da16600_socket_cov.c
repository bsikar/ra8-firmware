/**
 * @file test_ra_da16600_socket_cov.c
 * @brief Coverage supplement for ra_da16600_socket.c
 *
 * @par Tag
 * [Ring 4 / Test] {World: NS}
 *
 * @details
 * Targets the 26 lines in ra_da16600_socket.c that remain uncovered
 * after the base test_ra_da16600.c and test_ra_da16600_mcdc.c runs.
 * Two groups:
 *
 *   Group A -- not-initialized returns (lines 295, 331, 382, 405, 430,
 *   440): the RA8D2 firmware module starts zeroed so these are reached
 *   by calling each public entry point before ra_da16600_init returns.
 *
 *   Group B -- cid-parser edge cases and error paths (lines 72, 73, 77,
 *   78, 83, 90, 91, 311, 349, 350, 395): reached after initialization
 *   by seeding specific modem mock responses or choosing boundary-length
 *   payloads.
 *
 * Nine lines (137, 138, 195, 196, 201, 202, 340, 414, 415) are marked
 * GCOVR_EXCL_LINE in the source; see the justification comments above
 * each marker.
 *
 * The mock harness is identical in shape to test_ra_da16600_mcdc.c:
 * two in-memory FIFOs stand in for the DA16600 UART and a fake
 * monotonic clock lets timeouts be forced deterministically.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_da16600.h"
#include "ra_da16600_internal.h"
#include "ra_err.h"
#include "unity_minimal.h"

/* -------------------------------------------------------------------------
 * Mock byte transport: two FIFOs + a fake monotonic clock.
 * (Same shape as test_ra_da16600.c and test_ra_da16600_mcdc.c.)
 * -------------------------------------------------------------------------
 */

/**
 * @enum da16600_cov_caps_t
 * @brief Sizing constants for the mock FIFO and caller-owned line buffer.
 *
 * @details All values are typed C23 enums so the buffer-size sweep tool
 * can prove the test frame budget without preprocessor expansion.
 */
typedef enum : uint16_t {
  k_da16600_cov_fifo_cap     = 2048U, /**< Bytes in each mock FIFO.    */
  k_da16600_cov_line_buf_len = 512U,  /**< AT line accumulator length. */
} da16600_cov_caps_t;

/**
 * @enum da16600_cov_lengths_t
 * @brief Boundary lengths used by overflow and payload-size tests.
 *
 * @details The TRTC overflow test needs a remote_ip string long enough
 * to push "AT+TRTC=<ip>,<port>" past the 96-byte command buffer; 90 'a'
 * chars gives "AT+TRTC="(8) + 90 = 98 bytes, overflowing the buffer.
 * The payload-overflow test needs a payload length that pushes
 * off + len + 1 past 96; sock=0 yields off=14, so len=82 gives 97.
 */
typedef enum : uint8_t {
  k_da16600_cov_long_ip_len          = 90U, /**< Over-long IP to overflow TRTC cmd.  */
  k_da16600_cov_payload_overflow_len = 82U, /**< Payload that overflows AT-line buf. */
} da16600_cov_lengths_t;

/**
 * @struct da16600_cov_fifo_t
 * @brief One direction of the mock UART.
 *
 * @invariant head <= tail <= k_da16600_cov_fifo_cap.
 */
typedef struct {
  uint8_t  buf[k_da16600_cov_fifo_cap]; /**< Backing storage.      */
  uint16_t head;                        /**< Next byte to pop.     */
  uint16_t tail;                        /**< Next free write slot. */
} da16600_cov_fifo_t;

/**
 * @struct da16600_cov_io_state_t
 * @brief Aggregate mock-transport state shared by the IO callbacks.
 *
 * @invariant fake_now_ms is monotonic non-decreasing within one test.
 */
typedef struct {
  da16600_cov_fifo_t modem_to_mcu;    /**< Bytes the modem "sends" to the driver. */
  da16600_cov_fifo_t mcu_to_modem;    /**< Bytes the driver transmits out.        */
  uint32_t           fake_now_ms;     /**< Current fake-clock value.              */
  uint32_t           auto_advance_ms; /**< Added to fake_now_ms on every read.    */
} da16600_cov_io_state_t;

/**
 * @var s_io
 * @brief Singleton mock-transport state for the coverage test binary.
 *
 * @warning Reset via reset_world() before each test that uses the modem.
 */
static da16600_cov_io_state_t s_io;

/** @brief Reset one FIFO direction to empty. */
static void fifo_reset(da16600_cov_fifo_t* f)
{
  f->head = 0U;
  f->tail = 0U;
}

/** @brief Append one byte to a FIFO (silently drops on overflow). */
static void fifo_push(da16600_cov_fifo_t* f, uint8_t b)
{
  if (f->tail < (uint16_t)k_da16600_cov_fifo_cap) {
    f->buf[f->tail] = b;
    ++f->tail;
  }
}

/** @brief Append a NUL-terminated string to a FIFO byte by byte. */
static void fifo_push_str(da16600_cov_fifo_t* f, const char* s)
{
  uint16_t i = 0U;
  while (s[i] != '\0') {
    fifo_push(f, (uint8_t)s[i]);
    ++i;
  }
}

/**
 * @brief Pop one byte from a FIFO.
 * @return 0 on success, -1 if empty.
 */
static int32_t fifo_pop(da16600_cov_fifo_t* f, uint8_t* out)
{
  if (f->head >= f->tail) {
    return -1;
  }
  *out = f->buf[f->head];
  ++f->head;
  return 0;
}

/** @brief Mock TX: write one byte from driver -> modem direction. */
static ra_err_t mock_tx(void* ctx, uint8_t byte)
{
  (void)ctx;
  fifo_push(&s_io.mcu_to_modem, byte);
  return k_ra_ok;
}

/** @brief Mock RX: read one byte from modem -> driver direction. */
static ra_err_t mock_rx(void* ctx, uint8_t* out)
{
  (void)ctx;
  if (fifo_pop(&s_io.modem_to_mcu, out) != 0) {
    return k_ra_err_no_data;
  }
  return k_ra_ok;
}

/** @brief Mock clock: advance by auto_advance_ms on every call. */
static uint32_t mock_now(void* ctx)
{
  (void)ctx;
  s_io.fake_now_ms += s_io.auto_advance_ms;
  return s_io.fake_now_ms;
}

/** @brief Return both FIFOs and the fake clock to their initial state. */
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
static uint8_t s_line_buf[k_da16600_cov_line_buf_len];

/**
 * @brief Reset the world and initialize the DA16600 driver via AT probe.
 *
 * @details Seeds the modem-side RX FIFO with the bare-AT "AT\r\n\r\nOK\r\n"
 * probe response so ra_da16600_init returns k_ra_ok.
 *
 * @return k_ra_ok on success, a propagated error code otherwise.
 *
 * @pre s_io has been declared.
 * @pre The module has not been torn down since the last reset.
 * @post s_da.initialized == 1.
 * @post s_io FIFOs are empty (probe response consumed).
 *
 * @note Not thread-safe; test suite is single-threaded.
 * @since 0.1.0
 */
static ra_err_t bring_up_with_probe(void)
{
  reset_world();
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
 * @details Used to build over-long strings that exceed the 96-byte AT
 * command buffer without repeating a memset pattern in every test.
 *
 * @param[out] dst Destination buffer; must hold at least @p n + 1 bytes.
 * @param[in]  n   Number of 'a' characters to write (0 is valid).
 *
 * @pre @p dst is non-NULL and points to at least @p n + 1 bytes.
 * @pre @p n fits within the destination buffer.
 * @post dst[n] == '\0'.
 * @post Every byte dst[0..n-1] == 'a'.
 *
 * @note Test helper only; not thread-safe.
 * @since 0.1.0
 */
static void fill_a(char* dst, uint8_t n)
{
  uint8_t i = 0U;
  while (i < n) {
    dst[i] = 'a';
    ++i;
  }
  dst[n] = '\0';
}

/* -------------------------------------------------------------------------
 * Group A: not-initialized return paths.
 *
 * The module starts zeroed in this binary (s_da.initialized == 0) so
 * every function that gates on ra_da16600_require_init() returns
 * k_ra_err_not_initialized before touching any IO. These tests run
 * before bring_up_with_probe() is ever called.
 * -------------------------------------------------------------------------
 */

/**
 * @test test_tcp_open_requires_init
 * @par MC/DC:
 * Single-condition gate: s_da.initialized == 0 -> k_ra_err_not_initialized.
 * No compound boolean. Covers line 295 of ra_da16600_socket.c.
 */
static void test_tcp_open_requires_init(void)
{
  TEST_BEGIN("da16600 socket_cov tcp_open before init -> not_initialized");
  ra_da16600_socket_t sock = 0xFFU;
  /* Listen role avoids the remote_ip null guard; out_socket is non-null. */
  TEST_ASSERT_EQ(k_ra_err_not_initialized,
                 ra_da16600_tcp_open(k_ra_da16600_socket_listen, nullptr, 7U, &sock));
  TEST_END("da16600 socket_cov tcp_open before init -> not_initialized");
}

/**
 * @test test_tcp_send_requires_init
 * @par MC/DC:
 * Single-condition gate: s_da.initialized == 0 -> k_ra_err_not_initialized.
 * No compound boolean. Covers line 331 of ra_da16600_socket.c.
 */
static void test_tcp_send_requires_init(void)
{
  TEST_BEGIN("da16600 socket_cov tcp_send before init -> not_initialized");
  uint8_t one_byte = 0xAAU;
  /* len=1 passes the size guards; data is non-null; require_init fires. */
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_da16600_tcp_send(0U, &one_byte, 1U));
  TEST_END("da16600 socket_cov tcp_send before init -> not_initialized");
}

/**
 * @test test_tcp_recv_requires_init
 * @par MC/DC:
 * Single-condition gate: s_da.initialized == 0 -> k_ra_err_not_initialized.
 * No compound boolean. Covers line 382 of ra_da16600_socket.c.
 */
static void test_tcp_recv_requires_init(void)
{
  TEST_BEGIN("da16600 socket_cov tcp_recv before init -> not_initialized");
  uint8_t buf[16] = {};
  size_t  out_len = 0U;
  /* buf non-null, out_len non-null, cap=16 != 0; require_init fires. */
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_da16600_tcp_recv(0U, buf, sizeof buf, &out_len, 50U));
  TEST_END("da16600 socket_cov tcp_recv before init -> not_initialized");
}

/**
 * @test test_tcp_close_requires_init
 * @par MC/DC:
 * Single-condition gate: s_da.initialized == 0 -> k_ra_err_not_initialized.
 * No compound boolean. Covers line 405 of ra_da16600_socket.c.
 */
static void test_tcp_close_requires_init(void)
{
  TEST_BEGIN("da16600 socket_cov tcp_close before init -> not_initialized");
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_da16600_tcp_close(0U));
  TEST_END("da16600 socket_cov tcp_close before init -> not_initialized");
}

/**
 * @test test_ble_start_requires_init
 * @par MC/DC:
 * Single-condition gate: s_da.initialized == 0 -> k_ra_err_not_initialized.
 * No compound boolean. Covers line 430 of ra_da16600_socket.c.
 */
static void test_ble_start_requires_init(void)
{
  TEST_BEGIN("da16600 socket_cov ble_advertise_start before init -> not_initialized");
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_da16600_ble_advertise_start());
  TEST_END("da16600 socket_cov ble_advertise_start before init -> not_initialized");
}

/**
 * @test test_ble_stop_requires_init
 * @par MC/DC:
 * Single-condition gate: s_da.initialized == 0 -> k_ra_err_not_initialized.
 * No compound boolean. Covers line 440 of ra_da16600_socket.c.
 */
static void test_ble_stop_requires_init(void)
{
  TEST_BEGIN("da16600 socket_cov ble_advertise_stop before init -> not_initialized");
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_da16600_ble_advertise_stop());
  TEST_END("da16600 socket_cov ble_advertise_stop before init -> not_initialized");
}

/* -------------------------------------------------------------------------
 * Group B: cid-parser edge cases and error paths (driver initialized).
 * -------------------------------------------------------------------------
 */

/**
 * @test test_tcp_open_cid_no_colon
 * @par MC/DC:
 * Decision `internal_parse_socket_cid`: `if (capture[i] != ':')` true.
 * The modem responds with just OK and no payload line, so the capture
 * buffer is empty ("") -- no colon present. The legacy-firmware path
 * sets *out_cid = 0 and returns k_ra_ok. Covers lines 72-73.
 */
static void test_tcp_open_cid_no_colon(void)
{
  TEST_BEGIN("da16600 socket_cov tcp_open cid no-colon -> legacy cid=0");
  reset_world();
  /* No payload line -- only an empty line and OK; capture stays "". */
  fifo_push_str(&s_io.modem_to_mcu, "\r\nOK\r\n");
  ra_da16600_socket_t sock = 0xFFU;
  TEST_ASSERT_EQ(k_ra_ok, ra_da16600_tcp_open(k_ra_da16600_socket_listen, nullptr, 7U, &sock));
  /* Legacy-firmware contract: cid absent -> *out_socket zeroed. */
  TEST_ASSERT_EQ(0, sock);
  TEST_END("da16600 socket_cov tcp_open cid no-colon -> legacy cid=0");
}

/**
 * @test test_tcp_open_cid_space_after_colon
 * @par MC/DC:
 * Decision `internal_parse_socket_cid`: `if (capture[i] == ' ')` true.
 * Some DA16600 firmware revisions insert a space between the colon and
 * the cid decimal (e.g. "+TRTS: 5"). Covers lines 77-78.
 */
static void test_tcp_open_cid_space_after_colon(void)
{
  TEST_BEGIN("da16600 socket_cov tcp_open cid space-after-colon parsed");
  reset_world();
  /* "+TRTS: 5" has a space between ':' and the digit '5'. */
  fifo_push_str(&s_io.modem_to_mcu, "AT+TRTS=7\r\n+TRTS: 5\r\n\r\nOK\r\n");
  ra_da16600_socket_t sock = 0xFFU;
  TEST_ASSERT_EQ(k_ra_ok, ra_da16600_tcp_open(k_ra_da16600_socket_listen, nullptr, 7U, &sock));
  TEST_ASSERT_EQ(5, sock);
  TEST_END("da16600 socket_cov tcp_open cid space-after-colon parsed");
}

/**
 * @test test_tcp_open_cid_nondigit_break
 * @par MC/DC:
 * Decision `internal_parse_socket_cid` inner loop:
 *   while (capture[i] >= '0') / if (capture[i] > '9') break
 * The payload "+TRTS:2x" places 'x' (ASCII 0x78 >= '0', > '9') after
 * the digit '2'. The outer while enters because 'x' >= '0'; the inner
 * if fires because 'x' > '9' and breaks. saw==1 from the '2', so the
 * function returns k_ra_ok with cid=2. Covers line 83.
 */
static void test_tcp_open_cid_nondigit_break(void)
{
  TEST_BEGIN("da16600 socket_cov tcp_open cid nondigit break in digit loop");
  reset_world();
  /* "2x" -> digit '2' is parsed, 'x' triggers the break on line 83. */
  fifo_push_str(&s_io.modem_to_mcu, "AT+TRTS=7\r\n+TRTS:2x\r\n\r\nOK\r\n");
  ra_da16600_socket_t sock = 0xFFU;
  TEST_ASSERT_EQ(k_ra_ok, ra_da16600_tcp_open(k_ra_da16600_socket_listen, nullptr, 7U, &sock));
  TEST_ASSERT_EQ(2, sock);
  TEST_END("da16600 socket_cov tcp_open cid nondigit break in digit loop");
}

/**
 * @test test_tcp_open_cid_no_digits
 * @par MC/DC:
 * Decision `internal_parse_socket_cid`: `if (saw == 0U)` true.
 * The payload "+TRTS:" has a colon but is immediately NUL-terminated;
 * the digit loop never executes (saw stays 0). The hw_error path fires.
 * Covers lines 90-91.
 */
static void test_tcp_open_cid_no_digits(void)
{
  TEST_BEGIN("da16600 socket_cov tcp_open cid colon-no-digits -> hw_error");
  reset_world();
  /* "+TRTS:" without a following digit -> saw=0 -> hw_error. */
  fifo_push_str(&s_io.modem_to_mcu, "AT+TRTS=7\r\n+TRTS:\r\n\r\nOK\r\n");
  ra_da16600_socket_t sock = 0xFFU;
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 ra_da16600_tcp_open(k_ra_da16600_socket_listen, nullptr, 7U, &sock));
  TEST_END("da16600 socket_cov tcp_open cid colon-no-digits -> hw_error");
}

/**
 * @test test_tcp_open_connect_overflow
 * @par MC/DC:
 * Decision `internal_build_tcp_open_cmd` TRTC 4-condition OR
 *   (strcat(AT+TRTC=) == 0) || (strcat(ip) == 0) || ...
 * A 90-char remote_ip makes "AT+TRTC=" (8) + 90 = 98 bytes overflow
 * the 96-byte command buffer; the second OR-arm fires returning
 * k_ra_err_invalid_size which propagates through line 311 in tcp_open.
 * Covers line 311.
 */
static void test_tcp_open_connect_overflow(void)
{
  TEST_BEGIN("da16600 socket_cov tcp_open TRTC overflow -> invalid_size");
  reset_world();
  char long_ip[(uint8_t)k_da16600_cov_long_ip_len + 1U];
  fill_a(long_ip, (uint8_t)k_da16600_cov_long_ip_len);
  ra_da16600_socket_t sock = 0xFFU;
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_tcp_open(k_ra_da16600_socket_connect, long_ip, 80U, &sock));
  TEST_END("da16600 socket_cov tcp_open TRTC overflow -> invalid_size");
}

/**
 * @test test_tcp_send_payload_exceeds_buf
 * @par MC/DC:
 * Decision `ra_da16600_tcp_send`:
 *   (off + len + 1U) > sizeof cmd
 * With sock=0 the TRDTS header is "AT+TRDTS=0,<len>," = 14 bytes
 * (off=14). Payload len=82 gives 14+82+1=97 > 96, overflowing the
 * AT-line buffer before any modem IO. Covers lines 349-350.
 */
static void test_tcp_send_payload_exceeds_buf(void)
{
  TEST_BEGIN("da16600 socket_cov tcp_send payload exceeds AT-line buffer");
  reset_world();
  uint8_t payload[(uint8_t)k_da16600_cov_payload_overflow_len] = {};
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_tcp_send(0U, payload, (uint8_t)k_da16600_cov_payload_overflow_len));
  TEST_END("da16600 socket_cov tcp_send payload exceeds AT-line buffer");
}

/**
 * @test test_tcp_recv_cmd_timeout
 * @par MC/DC:
 * Decision `internal_wait_response`:
 *   elapsed >= timeout_ms
 * The RX FIFO is empty and auto_advance_ms=5 > timeout_ms=1, so the
 * elapsed check fires after the first clock read in the wait loop.
 * ra_modem_at_send_cmd_capture returns k_ra_err_hw_timeout which
 * propagates through line 395 of tcp_recv. Covers line 395.
 */
static void test_tcp_recv_cmd_timeout(void)
{
  TEST_BEGIN("da16600 socket_cov tcp_recv AT timeout propagated");
  reset_world();
  /* Clock advances 5 ms per tick; timeout_ms=1 fires immediately. */
  s_io.auto_advance_ms = 5U;
  uint8_t buf[16]      = {};
  size_t  out_len      = 7U;
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, ra_da16600_tcp_recv(0U, buf, sizeof buf, &out_len, 1U));
  /* out_len must be cleared to 0 on any error path. */
  TEST_ASSERT_EQ(0, out_len);
  TEST_END("da16600 socket_cov tcp_recv AT timeout propagated");
}

/* -------------------------------------------------------------------------
 * Test entry point.
 * -------------------------------------------------------------------------
 */

/**
 * @test test_build_tcp_open_cmd_overflow_mcdc
 * @par MC/DC:
 * Two decisions in `ra_da16600_build_tcp_open_cmd`, each exercised by calling
 * the promoted builder directly with a cmd_cap sized so exactly one append
 * fails (the production 96-byte buffer never overflows on the fixed-shape
 * commands):
 * - Listen OR (2 conditions): cap=96 F,F (ok); cap=5 T ("AT+TRTS=" fails);
 *   cap=9 F,T (port append fails).
 * - Connect OR (4 conditions): cap=96 F,F,F,F (ok); cap=5 T ("AT+TRTC=" fails);
 *   cap=9 F,T (remote_ip fails); cap=16 F,F,T ("," fails); cap=17 F,F,F,T
 *   (port fails). Each vector isolates one condition. N+1 minimal MC/DC.
 */
static void test_build_tcp_open_cmd_overflow_mcdc(void)
{
  TEST_BEGIN("da16600 build_tcp_open_cmd overflow ORs (MC/DC)");
  char cmd[k_ra_da16600_cmd_buf_bytes];
  /* Listen role (2-condition OR): remote_ip unused. */
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_da16600_build_tcp_open_cmd(cmd,
                                               sizeof cmd,
                                               k_ra_da16600_socket_listen,
                                               nullptr,
                                               80U)); /* F,F */
  TEST_ASSERT_EQ(
    k_ra_err_invalid_size,
    ra_da16600_build_tcp_open_cmd(cmd, 5U, k_ra_da16600_socket_listen, nullptr, 80U)); /* T */
  TEST_ASSERT_EQ(
    k_ra_err_invalid_size,
    ra_da16600_build_tcp_open_cmd(cmd, 9U, k_ra_da16600_socket_listen, nullptr, 80U)); /* F,T */
  /* Connect role (4-condition OR). */
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_da16600_build_tcp_open_cmd(cmd,
                                               sizeof cmd,
                                               k_ra_da16600_socket_connect,
                                               "1.2.3.4",
                                               80U)); /* F,F,F,F */
  TEST_ASSERT_EQ(
    k_ra_err_invalid_size,
    ra_da16600_build_tcp_open_cmd(cmd, 5U, k_ra_da16600_socket_connect, "1.2.3.4", 80U)); /* T */
  TEST_ASSERT_EQ(
    k_ra_err_invalid_size,
    ra_da16600_build_tcp_open_cmd(cmd, 9U, k_ra_da16600_socket_connect, "1.2.3.4", 80U)); /* F,T */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_build_tcp_open_cmd(cmd,
                                               16U,
                                               k_ra_da16600_socket_connect,
                                               "1.2.3.4",
                                               80U)); /* F,F,T */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_build_tcp_open_cmd(cmd,
                                               17U,
                                               k_ra_da16600_socket_connect,
                                               "1.2.3.4",
                                               80U)); /* F,F,F,T */
  TEST_END("da16600 build_tcp_open_cmd overflow ORs (MC/DC)");
}

/**
 * @test test_build_trdts_header_overflow_mcdc
 * @par MC/DC:
 * Two decisions in `ra_da16600_build_trdts_header` (sock=5, len=99):
 * - First OR (3 conditions, "AT+TRDTS=" / cid / ","): cap=96 F,F,F (ok);
 *   cap=5 T (prefix fails); cap=10 F,T (cid fails); cap=11 F,F,T ("," fails).
 * - Second OR (2 conditions, len / ","): cap=96 F,F (ok, shared with above);
 *   cap=12 T (len append fails); cap=14 F,T (trailing "," fails). Reached only
 *   after the first OR passes. N+1 minimal MC/DC for each.
 */
static void test_build_trdts_header_overflow_mcdc(void)
{
  TEST_BEGIN("da16600 build_trdts_header overflow ORs (MC/DC)");
  char   cmd[k_ra_da16600_cmd_buf_bytes];
  size_t off = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_da16600_build_trdts_header(cmd, sizeof cmd, 5U, 99U, &off)); /* FFF / FF */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_build_trdts_header(cmd, 5U, 5U, 99U, &off)); /* 3-OR T */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_build_trdts_header(cmd, 10U, 5U, 99U, &off)); /* 3-OR F,T */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_build_trdts_header(cmd, 11U, 5U, 99U, &off)); /* 3-OR F,F,T */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_build_trdts_header(cmd, 12U, 5U, 99U, &off)); /* 2-OR T */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_da16600_build_trdts_header(cmd, 14U, 5U, 99U, &off)); /* 2-OR F,T */
  TEST_END("da16600 build_trdts_header overflow ORs (MC/DC)");
}

/**
 * @test test_build_trtrm_cmd_overflow_mcdc
 * @par MC/DC:
 * Decision `ra_da16600_build_trtrm_cmd` (2-condition OR, sock=5):
 * - V1: cap=96 F,F (ok).
 * - V2: cap=5 < len("AT+TRTRM=")=9 -> T (prefix append fails).
 * - V3: cap=10 fits the prefix only -> F,T (cid append fails).
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_build_trtrm_cmd_overflow_mcdc(void)
{
  TEST_BEGIN("da16600 build_trtrm_cmd overflow OR (MC/DC)");
  char cmd[k_ra_da16600_cmd_buf_bytes];
  TEST_ASSERT_EQ(k_ra_ok, ra_da16600_build_trtrm_cmd(cmd, sizeof cmd, 5U));        /* F,F */
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_da16600_build_trtrm_cmd(cmd, 5U, 5U));  /* T   */
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_da16600_build_trtrm_cmd(cmd, 10U, 5U)); /* F,T */
  TEST_END("da16600 build_trtrm_cmd overflow OR (MC/DC)");
}

int32_t main(void)
{
  test_build_tcp_open_cmd_overflow_mcdc();
  test_build_trdts_header_overflow_mcdc();
  test_build_trtrm_cmd_overflow_mcdc();

  /* Group A: not-initialized paths.  Must run before any bring_up call. */
  test_tcp_open_requires_init();
  test_tcp_send_requires_init();
  test_tcp_recv_requires_init();
  test_tcp_close_requires_init();
  test_ble_start_requires_init();
  test_ble_stop_requires_init();

  /* Initialize the driver once for all Group B tests. */
  TEST_ASSERT_EQ(k_ra_ok, bring_up_with_probe());

  /* Group B: cid-parser edge cases and error propagation paths. */
  test_tcp_open_cid_no_colon();
  test_tcp_open_cid_space_after_colon();
  test_tcp_open_cid_nondigit_break();
  test_tcp_open_cid_no_digits();
  test_tcp_open_connect_overflow();
  test_tcp_send_payload_exceeds_buf();
  test_tcp_recv_cmd_timeout();

  (void)fprintf(stderr, "[OK ] test_ra_da16600_socket_cov.c\n");
  return 0;
}
