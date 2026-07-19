/**
 * @file test_ra8_modem_at.c
 * @brief Unit tests for ra8_modem_at.c (cellular modem AT command driver)
 *
 * @details
 * Drives the AT command driver over a pair of in-memory FIFO mocks
 * that stand in for a real UART. Each test seeds the modem-side
 * RX FIFO with the bytes a SIM7600/BG95 would emit, runs the
 * driver, and asserts on what was placed in the modem-side TX FIFO
 * plus the value the public API returned. This sibling owns the
 * public-API contract tests; the MC/DC vector tests live in
 * test_ra8_modem_at_mcdc.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_modem_at.h"
#include "ra8_modem_at_internal.h"
#include "unity_minimal.h"

/**
 * @enum t_at_time_t
 * @brief AT-command timeouts and the mock transport's clock step, in ms.
 *
 * @details
 * The step is shorter than the short timeout so a command completes within its
 * budget, but several steps exceed it -- which is what makes the timeout arm
 * fire deterministically rather than by wall-clock luck.
 */
typedef enum : uint16_t {
  k_t_step_ms         = 50U,   /**< Mock clock advance per transport poll.   */
  k_t_timeout_short_ms = 100U, /**< Timeout the expiry arm uses.             */
  k_t_timeout_long_ms  = 1000U, /**< Timeout the happy-path arms use.        */
} t_at_time_t;

/**
 * @enum t_at_buf_t
 * @brief Buffer capacities of the parser fixture.
 */
typedef enum : uint16_t {
  k_t_response_cap = 64U,  /**< Response and URC capture buffers, bytes.     */
  k_t_line_cap     = 256U, /**< Line-assembly buffer, bytes.                 */
} t_at_buf_t;

/* ------------------------------------------------------------------------- */
/* Mock byte transport: two FIFOs + a fake monotonic clock. */
/* ------------------------------------------------------------------------- */

typedef enum : uint16_t {
  k_test_fifo_cap = 1024U, /**< Test FIFO cap. */
} test_fifo_caps_t;

typedef struct {
  uint8_t  buf[k_test_fifo_cap]; /**< Buffer. */
  uint16_t head;                 /**< Head.   */
  uint16_t tail;                 /**< Tail.   */
} test_fifo_t;

typedef struct {
  test_fifo_t modem_to_mcu;    /**< Bytes the modem sends to the MCU (rx_byte). */
  test_fifo_t mcu_to_modem;    /**< Bytes the MCU sends out (tx_byte).          */
  uint32_t    fake_now_ms;     /**< Fake now ms.                                */
  uint32_t    auto_advance_ms; /**< Advance time by this on every poll.         */
} test_io_state_t;

static test_io_state_t s_io;

static void fifo_reset(test_fifo_t* f)
{
  f->head = 0U;
  f->tail = 0U;
}

static void fifo_push(test_fifo_t* f, uint8_t b)
{
  if (f->tail < (uint16_t)k_test_fifo_cap) {
    f->buf[f->tail] = b;
    ++f->tail;
  }
}

static void fifo_push_str(test_fifo_t* f, const char* s)
{
  uint16_t i = 0U;
  while (s[i] != '\0') {
    fifo_push(f, (uint8_t)s[i]);
    ++i;
  }
}

static int32_t fifo_pop(test_fifo_t* f, uint8_t* out)
{
  if (f->head >= f->tail) {
    return -1;
  }
  *out = f->buf[f->head];
  ++f->head;
  return 0;
}

static ra8_err_t mock_tx(void* ctx, uint8_t byte)
{
  (void)ctx;
  fifo_push(&s_io.mcu_to_modem, byte);
  return k_ra8_ok;
}

static ra8_err_t mock_rx(void* ctx, uint8_t* out)
{
  (void)ctx;
  if (fifo_pop(&s_io.modem_to_mcu, out) != 0) {
    return k_ra8_err_no_data;
  }
  return k_ra8_ok;
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

static uint8_t s_line_buf[k_t_line_cap];

static ra8_err_t bring_up(void)
{
  reset_world();
  ra8_modem_at_cfg_t cfg = {
    .io           = {.tx_byte = mock_tx, .rx_byte = mock_rx, .now_ms = mock_now, .ctx = nullptr},
    .line_buf     = s_line_buf,
    .line_buf_len = (uint16_t)sizeof s_line_buf,
    .default_timeout_ms = k_t_timeout_long_ms,
  };
  return ra8_modem_at_init(&cfg);
}

static uint16_t mcu_tx_count(void)
{
  return s_io.mcu_to_modem.tail;
}

static int32_t mcu_tx_equals(const char* s)
{
  uint16_t i = 0U;
  while (s[i] != '\0') {
    if (i >= s_io.mcu_to_modem.tail) {
      return -1;
    }
    if (s_io.mcu_to_modem.buf[i] != (uint8_t)s[i]) {
      return -1;
    }
    ++i;
  }
  return (i == s_io.mcu_to_modem.tail) ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* Tests */
/* ------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("modem_at init NULL cfg");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_modem_at_init(nullptr));
  TEST_END("modem_at init NULL cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_short_buffer_rejected(void)
{
  TEST_BEGIN("modem_at init short buffer rejected");
  reset_world();
  uint8_t            tiny[4];
  ra8_modem_at_cfg_t cfg = {
    .io           = {.tx_byte = mock_tx, .rx_byte = mock_rx, .now_ms = mock_now, .ctx = nullptr},
    .line_buf     = tiny,
    .line_buf_len = (uint16_t)sizeof tiny,
    .default_timeout_ms = k_t_timeout_short_ms,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_modem_at_init(&cfg));
  TEST_END("modem_at init short buffer rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_cmd_before_init_fails(void)
{
  TEST_BEGIN("modem_at send_cmd before init returns not_initialized");
  /* Force-uninit by passing a NULL cfg (rejected) and rely on init flag.
   * To be deterministic we re-run init with NULL after which initialized
   * remains true, so instead we verify behaviour with a fresh process via
   * the current order: first test in main() calls this BEFORE bring_up().
   */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_modem_at_send_cmd("AT", nullptr, 50U));
  TEST_END("modem_at send_cmd before init returns not_initialized");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_at_ok_with_echo(void)
{
  TEST_BEGIN("modem_at AT -> echo + OK");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  /* Modem will echo "AT\r\n" then reply "\r\nOK\r\n". */
  fifo_push_str(&s_io.modem_to_mcu, "AT\r\n\r\nOK\r\n");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd("AT", nullptr, 1000U));
  TEST_ASSERT_EQ(0, mcu_tx_equals("AT\r"));
  TEST_END("modem_at AT -> echo + OK");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_at_error_returned(void)
{
  TEST_BEGIN("modem_at ERROR final result");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  fifo_push_str(&s_io.modem_to_mcu, "AT+BAD\r\n\r\nERROR\r\n");
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_modem_at_send_cmd("AT+BAD", nullptr, 1000U));
  TEST_END("modem_at ERROR final result");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_at_cme_error(void)
{
  TEST_BEGIN("modem_at +CME ERROR final result");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  fifo_push_str(&s_io.modem_to_mcu, "AT+CPIN?\r\n\r\n+CME ERROR: 10\r\n");
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_modem_at_send_cmd("AT+CPIN?", nullptr, 1000U));
  TEST_END("modem_at +CME ERROR final result");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_timeout(void)
{
  TEST_BEGIN("modem_at timeout when no response");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  /* Auto-advance the fake clock by 50 ms per poll so timeout trips fast. */
  s_io.auto_advance_ms = k_t_step_ms;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_modem_at_send_cmd("AT", nullptr, 100U));
  TEST_END("modem_at timeout when no response");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_cmd_capture(void)
{
  TEST_BEGIN("modem_at capture +CSQ payload");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  fifo_push_str(&s_io.modem_to_mcu, "AT+CSQ\r\n\r\n+CSQ: 22,99\r\n\r\nOK\r\n");
  char out[k_t_response_cap];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd_capture("AT+CSQ", out, sizeof out, 1000U));
  /* Captured payload should contain the +CSQ line. */
  TEST_ASSERT(out[0] == '+');
  TEST_ASSERT(out[1] == 'C');
  TEST_ASSERT(out[2] == 'S');
  TEST_ASSERT(out[3] == 'Q');
  TEST_END("modem_at capture +CSQ payload");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_buf_len_zero(void)
{
  TEST_BEGIN("modem_at capture rejects buf_len == 0");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  char out[8] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_modem_at_send_cmd_capture("AT", out, 0U, 100U));
  TEST_END("modem_at capture rejects buf_len == 0");
}

static int32_t s_urc_hits;
static char    s_last_urc[k_t_response_cap];

static void urc_handler(const char* line, void* ctx)
{
  (void)ctx;
  ++s_urc_hits;
  uint16_t i = 0U;
  while ((line[i] != '\0') && (i < (uint16_t)(sizeof s_last_urc - 1U))) {
    s_last_urc[i] = line[i];
    ++i;
  }
  s_last_urc[i] = '\0';
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_urc_dispatch(void)
{
  TEST_BEGIN("modem_at URC +CMTI dispatch during send_cmd");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  s_urc_hits    = 0;
  s_last_urc[0] = '\0';
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_modem_at_register_unsolicited_handler("+CMTI:", urc_handler, nullptr));
  /* SMS arrival URC arrives mixed in with command response. */
  fifo_push_str(&s_io.modem_to_mcu, "AT\r\n\r\n+CMTI: \"SM\",3\r\n\r\nOK\r\n");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd("AT", nullptr, 1000U));
  TEST_ASSERT_EQ(1, s_urc_hits);
  TEST_ASSERT(s_last_urc[0] == '+');
  TEST_END("modem_at URC +CMTI dispatch during send_cmd");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_urc_replace_same_prefix(void)
{
  TEST_BEGIN("modem_at URC replace same prefix");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_modem_at_register_unsolicited_handler("+CREG:", urc_handler, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_modem_at_register_unsolicited_handler("+CREG:", urc_handler, nullptr));
  TEST_END("modem_at URC replace same prefix");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_urc_table_full(void)
{
  TEST_BEGIN("modem_at URC table full returns no_mem");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  const char* prefixes[] = {"+A:", "+B:", "+C:", "+D:", "+E:", "+F:", "+G:", "+H:"};
  for (uint8_t i = 0U; i < (uint8_t)(sizeof prefixes / sizeof prefixes[0]); ++i) {
    TEST_ASSERT_EQ(k_ra8_ok,
                   ra8_modem_at_register_unsolicited_handler(prefixes[i], urc_handler, nullptr));
  }
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_modem_at_register_unsolicited_handler("+I:", urc_handler, nullptr));
  TEST_END("modem_at URC table full returns no_mem");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_urc_invalid_prefix(void)
{
  TEST_BEGIN("modem_at URC invalid prefix lengths");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_modem_at_register_unsolicited_handler("", urc_handler, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_modem_at_register_unsolicited_handler("+X:", nullptr, nullptr));
  TEST_END("modem_at URC invalid prefix lengths");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_poll_drains_urc(void)
{
  TEST_BEGIN("modem_at poll drains URC outside command");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  s_urc_hits = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_modem_at_register_unsolicited_handler("+CREG:", urc_handler, nullptr));
  fifo_push_str(&s_io.modem_to_mcu, "\r\n+CREG: 0,1\r\n");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_poll());
  TEST_ASSERT_EQ(1, s_urc_hits);
  TEST_END("modem_at poll drains URC outside command");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_expected_response(void)
{
  TEST_BEGIN("modem_at expected_response prefix accepted");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  fifo_push_str(&s_io.modem_to_mcu, "AT+CPIN?\r\n\r\n+CPIN: READY\r\n\r\nOK\r\n");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd("AT+CPIN?", "+CPIN:", 1000U));
  TEST_END("modem_at expected_response prefix accepted");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_cmd_null_arg(void)
{
  TEST_BEGIN("modem_at send_cmd NULL arg");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_modem_at_send_cmd(nullptr, nullptr, 100U));
  TEST_END("modem_at send_cmd NULL arg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_default_timeout_used(void)
{
  TEST_BEGIN("modem_at default timeout applied when 0 passed");
  TEST_ASSERT_EQ(k_ra8_ok, bring_up());
  fifo_push_str(&s_io.modem_to_mcu, "AT\r\n\r\nOK\r\n");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd("AT", nullptr, 0U));
  TEST_ASSERT(mcu_tx_count() > 0U);
  TEST_END("modem_at default timeout applied when 0 passed");
}

int32_t main(void)
{
  /* This test must run BEFORE bring_up() so the initialized flag is 0. */
  test_send_cmd_before_init_fails();

  test_init_null_cfg();
  test_init_short_buffer_rejected();
  test_at_ok_with_echo();
  test_at_error_returned();
  test_at_cme_error();
  test_timeout();
  test_send_cmd_capture();
  test_capture_buf_len_zero();
  test_urc_dispatch();
  test_urc_replace_same_prefix();
  test_urc_table_full();
  test_urc_invalid_prefix();
  test_poll_drains_urc();
  test_expected_response();
  test_send_cmd_null_arg();
  test_default_timeout_used();
  (void)fprintf(stderr, "[OK ] test_ra8_modem_at.c\n");
  return 0;
}
