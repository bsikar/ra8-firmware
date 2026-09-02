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

#include "ra8_attributes.h"
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
  k_t_step_ms          = 50U,   /**< Mock clock advance per transport poll. */
  k_t_timeout_short_ms = 100U,  /**< Timeout the expiry arm uses.           */
  k_t_timeout_long_ms  = 1000U, /**< Timeout the happy-path arms use.       */
} t_at_time_t;

/**
 * @enum t_at_buf_t
 * @brief Buffer capacities of the parser fixture.
 */
typedef enum : uint16_t {
  k_t_response_cap = 64U,  /**< Response and URC capture buffers, bytes. */
  k_t_line_cap     = 256U, /**< Line-assembly buffer, bytes.             */
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

/**
 * @brief Reset one test FIFO to the empty state.
 * @details Sets both cursor indices to zero without reading or rewriting the
 * backing bytes.
 * @param[in,out] f FIFO owned by the current test.
 * @pre @p f is non-NULL.
 * @pre No concurrent code accesses @p f.
 * @post The FIFO head is zero.
 * @post The FIFO tail is zero.
 * @note Backing bytes retain unspecified prior values.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_fifo_reset(test_fifo_t* f)
{
  f->head = 0U;
  f->tail = 0U;
}

/**
 * @brief Append one byte to a test FIFO when capacity remains.
 * @details Advances the tail only after storing the byte; a full FIFO is left
 * unchanged.
 * @param[in,out] f FIFO owned by the current test.
 * @param[in] b Byte to append.
 * @pre @p f is non-NULL.
 * @pre The FIFO indices describe its backing array.
 * @post The byte is appended when the FIFO is not full.
 * @post A full FIFO retains its prior indices and bytes.
 * @note This fixture intentionally models loss on overflow.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_fifo_push(test_fifo_t* f, uint8_t b)
{
  if (f->tail < (uint16_t)k_test_fifo_cap) {
    f->buf[f->tail] = b;
    ++f->tail;
  }
}

/**
 * @brief Append a NUL-terminated string to a test FIFO.
 * @details Feeds each source byte through @ref internal_fifo_push and omits the
 * terminator.
 * @param[in,out] f FIFO owned by the current test.
 * @param[in] s NUL-terminated byte string to enqueue.
 * @pre @p f is non-NULL.
 * @pre @p s is non-NULL and NUL-terminated.
 * @post Each byte that fits has been offered to the FIFO in order.
 * @post The source string is unchanged.
 * @note Capacity handling is inherited from @ref internal_fifo_push.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_fifo_push_str(test_fifo_t* f, const char* s)
{
  uint16_t i = 0U;
  while (s[i] != '\0') {
    internal_fifo_push(f, (uint8_t)s[i]);
    ++i;
  }
}

/**
 * @brief Remove the next byte from a test FIFO.
 * @details Returns a negative sentinel without writing @p out when the FIFO is
 * empty.
 * @param[in,out] f FIFO owned by the current test.
 * @param[out] out Destination for one available byte.
 * @return Zero when a byte is returned, otherwise negative one.
 * @retval 0 One byte was stored in @p out.
 * @retval -1 The FIFO was empty and @p out was not written.
 * @pre @p f is non-NULL.
 * @pre @p out is non-NULL.
 * @post A successful pop advances the head by one.
 * @post An empty pop preserves the FIFO and destination.
 * @note This is the receive callback's nonblocking source.
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t internal_fifo_pop(test_fifo_t* f, uint8_t* out)
{
  if (f->head >= f->tail) {
    return -1;
  }
  *out = f->buf[f->head];
  ++f->head;
  return 0;
}

/**
 * @brief Capture one modem-transmit byte in the fixture FIFO.
 * @details Implements the configured transmit callback with deterministic
 * in-memory storage.
 * @param[in] ctx Unused transport context.
 * @param[in] byte Byte emitted by the driver.
 * @return The callback result.
 * @retval k_ra8_ok The byte was accepted by the fixture.
 * @pre The fixture state has been reset for the current test.
 * @pre The transmit FIFO indices are valid.
 * @post The byte has been offered to the transmit FIFO.
 * @post No external I/O has occurred.
 * @note @p ctx is intentionally unused by the singleton fixture.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_mock_tx(void* ctx, uint8_t byte)
{
  (void)ctx;
  internal_fifo_push(&s_io.mcu_to_modem, byte);
  return k_ra8_ok;
}

/**
 * @brief Supply one queued receive byte to the modem driver.
 * @details Adapts @ref internal_fifo_pop to the modem transport error contract.
 * @param[in] ctx Unused transport context.
 * @param[out] out Destination for the next received byte.
 * @return The callback result.
 * @retval k_ra8_ok A byte was returned.
 * @retval k_ra8_err_no_data No receive byte was queued.
 * @pre @p out is non-NULL.
 * @pre The receive FIFO indices are valid.
 * @post Success advances the receive FIFO head.
 * @post No-data preserves @p out and the FIFO.
 * @note @p ctx is intentionally unused by the singleton fixture.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_mock_rx(void* ctx, uint8_t* out)
{
  (void)ctx;
  if (internal_fifo_pop(&s_io.modem_to_mcu, out) != 0) {
    return k_ra8_err_no_data;
  }
  return k_ra8_ok;
}

/**
 * @brief Advance and return the fixture's monotonic clock.
 * @details Adds the configured per-read increment before returning the new
 * timestamp.
 * @param[in] ctx Unused transport context.
 * @return Current fake time in milliseconds.
 * @retval 0 The clock remains at its reset value when automatic advance is
 * zero.
 * @pre The fixture state has been initialized.
 * @pre No concurrent code mutates the fake clock.
 * @post Fake time advances by exactly the configured increment modulo 32 bits.
 * @post No wall-clock source has been queried.
 * @note @p ctx is intentionally unused by the singleton fixture.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_mock_now(void* ctx)
{
  (void)ctx;
  s_io.fake_now_ms += s_io.auto_advance_ms;
  return s_io.fake_now_ms;
}

/**
 * @brief Reset every in-memory transport field.
 * @details Empties both FIFOs and restores the fake clock and increment to
 * zero.
 * @pre The fixture exclusively owns @ref s_io.
 * @pre Both FIFO objects have valid backing arrays.
 * @post Both transport FIFOs are empty.
 * @post Fake time and automatic advance are zero.
 * @note The modem module itself is initialized separately.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_reset_world(void)
{
  internal_fifo_reset(&s_io.modem_to_mcu);
  internal_fifo_reset(&s_io.mcu_to_modem);
  s_io.fake_now_ms     = 0U;
  s_io.auto_advance_ms = 0U;
}

static uint8_t s_line_buf[k_t_line_cap];

/**
 * @brief Initialize the modem driver against the reset fixture.
 * @details Resets transport state and binds the static line buffer and callback
 * set.
 * @return The modem initialization result.
 * @retval k_ra8_ok The fixture configuration was accepted.
 * @pre The fixture owns @ref s_line_buf for the test duration.
 * @pre No modem command is active.
 * @post Transport state is reset.
 * @post Success leaves the modem initialized with the fixture callbacks.
 * @note Tests assert the returned status before continuing.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_bring_up(void)
{
  internal_reset_world();
  ra8_modem_at_cfg_t cfg = {
    .io                 = {.tx_byte = internal_mock_tx,
                           .rx_byte = internal_mock_rx,
                           .now_ms  = internal_mock_now,
                           .ctx     = nullptr},
    .line_buf           = s_line_buf,
    .line_buf_len       = (uint16_t)sizeof s_line_buf,
    .default_timeout_ms = k_t_timeout_long_ms,
  };
  return ra8_modem_at_init(&cfg);
}

/**
 * @brief Return the number of bytes transmitted by the modem driver.
 * @details Reads the fixture FIFO tail, which equals its populated byte count
 * after reset.
 * @return Transmitted byte count.
 * @retval 0 No byte has been transmitted since reset.
 * @pre The transmit FIFO indices are valid.
 * @pre No concurrent callback mutates the FIFO.
 * @post The FIFO is unchanged.
 * @post The returned value equals the current tail index.
 * @note The fixture never compacts its FIFO.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_mcu_tx_count(void)
{
  return s_io.mcu_to_modem.tail;
}

/**
 * @brief Compare captured transmit bytes with a NUL-terminated string.
 * @details Requires byte-for-byte equality and an exact length match.
 * @param[in] s Expected NUL-terminated command text.
 * @return Zero on exact equality, otherwise negative one.
 * @retval 0 Captured bytes exactly equal @p s.
 * @retval -1 A byte or length differs.
 * @pre @p s is non-NULL and NUL-terminated.
 * @pre The transmit FIFO indices are valid.
 * @post The fixture and expected string are unchanged.
 * @post The return value reflects the full captured sequence.
 * @note The terminating NUL is not expected in the FIFO.
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t internal_mcu_tx_equals(const char* s)
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
 * @brief Exercise the @c internal_test_init_null_cfg scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_init_null_cfg(void)
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
 * @brief Exercise the @c internal_test_init_short_buffer_rejected scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_init_short_buffer_rejected(void)
{
  TEST_BEGIN("modem_at init short buffer rejected");
  internal_reset_world();
  uint8_t            tiny[4];
  ra8_modem_at_cfg_t cfg = {
    .io                 = {.tx_byte = internal_mock_tx,
                           .rx_byte = internal_mock_rx,
                           .now_ms  = internal_mock_now,
                           .ctx     = nullptr},
    .line_buf           = tiny,
    .line_buf_len       = (uint16_t)sizeof tiny,
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
 * @brief Exercise the @c internal_test_send_cmd_before_init_fails scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_send_cmd_before_init_fails(void)
{
  TEST_BEGIN("modem_at send_cmd before init returns not_initialized");
  /* Force-uninit by passing a NULL cfg (rejected) and rely on init flag.
   * To be deterministic we re-run init with NULL after which initialized
   * remains true, so instead we verify behaviour with a fresh process via
   * the current order: first test in main() calls this BEFORE
   * internal_bring_up().
   */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_modem_at_send_cmd("AT", nullptr, 50U));
  TEST_END("modem_at send_cmd before init returns not_initialized");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Exercise the @c internal_test_at_ok_with_echo scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_at_ok_with_echo(void)
{
  TEST_BEGIN("modem_at AT -> echo + OK");
  TEST_ASSERT_EQ(k_ra8_ok, internal_bring_up());
  /* Modem will echo "AT\r\n" then reply "\r\nOK\r\n". */
  internal_fifo_push_str(&s_io.modem_to_mcu, "AT\r\n\r\nOK\r\n");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd("AT", nullptr, 1000U));
  TEST_ASSERT_EQ(0, internal_mcu_tx_equals("AT\r"));
  TEST_END("modem_at AT -> echo + OK");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Exercise the @c internal_test_at_error_returned scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_at_error_returned(void)
{
  TEST_BEGIN("modem_at ERROR final result");
  TEST_ASSERT_EQ(k_ra8_ok, internal_bring_up());
  internal_fifo_push_str(&s_io.modem_to_mcu, "AT+BAD\r\n\r\nERROR\r\n");
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_modem_at_send_cmd("AT+BAD", nullptr, 1000U));
  TEST_END("modem_at ERROR final result");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Exercise the @c internal_test_at_cme_error scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_at_cme_error(void)
{
  TEST_BEGIN("modem_at +CME ERROR final result");
  TEST_ASSERT_EQ(k_ra8_ok, internal_bring_up());
  internal_fifo_push_str(&s_io.modem_to_mcu, "AT+CPIN?\r\n\r\n+CME ERROR: 10\r\n");
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_modem_at_send_cmd("AT+CPIN?", nullptr, 1000U));
  TEST_END("modem_at +CME ERROR final result");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Exercise the @c internal_test_timeout scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_timeout(void)
{
  TEST_BEGIN("modem_at timeout when no response");
  TEST_ASSERT_EQ(k_ra8_ok, internal_bring_up());
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
 * @brief Exercise the @c internal_test_send_cmd_capture scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_send_cmd_capture(void)
{
  TEST_BEGIN("modem_at capture +CSQ payload");
  TEST_ASSERT_EQ(k_ra8_ok, internal_bring_up());
  internal_fifo_push_str(&s_io.modem_to_mcu, "AT+CSQ\r\n\r\n+CSQ: 22,99\r\n\r\nOK\r\n");
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
 * @brief Exercise the @c internal_test_capture_buf_len_zero scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_capture_buf_len_zero(void)
{
  TEST_BEGIN("modem_at capture rejects buf_len == 0");
  TEST_ASSERT_EQ(k_ra8_ok, internal_bring_up());
  char out[8] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_modem_at_send_cmd_capture("AT", out, 0U, 100U));
  TEST_END("modem_at capture rejects buf_len == 0");
}

static int32_t s_urc_hits;
static char    s_last_urc[k_t_response_cap];

/**
 * @brief Record one dispatched unsolicited response.
 * @details Increments the hit count and copies a bounded NUL-terminated prefix
 * into the fixture.
 * @param[in] line NUL-terminated unsolicited response line.
 * @param[in] ctx Unused callback context.
 * @pre @p line is non-NULL and NUL-terminated.
 * @pre The fixture exclusively owns the callback state.
 * @post The hit count has advanced by one.
 * @post The captured line is NUL-terminated within its fixed buffer.
 * @note Truncation is intentional and bounded by @ref s_last_urc.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_urc_handler(const char* line, void* ctx)
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
 * @brief Exercise the @c internal_test_urc_dispatch scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_urc_dispatch(void)
{
  TEST_BEGIN("modem_at URC +CMTI dispatch during send_cmd");
  TEST_ASSERT_EQ(k_ra8_ok, internal_bring_up());
  s_urc_hits    = 0;
  s_last_urc[0] = '\0';
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_modem_at_register_unsolicited_handler("+CMTI:", internal_urc_handler, nullptr));
  /* SMS arrival URC arrives mixed in with command response. */
  internal_fifo_push_str(&s_io.modem_to_mcu, "AT\r\n\r\n+CMTI: \"SM\",3\r\n\r\nOK\r\n");
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
 * @brief Exercise the @c internal_test_urc_replace_same_prefix scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_urc_replace_same_prefix(void)
{
  TEST_BEGIN("modem_at URC replace same prefix");
  TEST_ASSERT_EQ(k_ra8_ok, internal_bring_up());
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_modem_at_register_unsolicited_handler("+CREG:", internal_urc_handler, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_modem_at_register_unsolicited_handler("+CREG:", internal_urc_handler, nullptr));
  TEST_END("modem_at URC replace same prefix");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Exercise the @c internal_test_urc_table_full scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_urc_table_full(void)
{
  TEST_BEGIN("modem_at URC table full returns no_mem");
  TEST_ASSERT_EQ(k_ra8_ok, internal_bring_up());
  const char* prefixes[] = {"+A:", "+B:", "+C:", "+D:", "+E:", "+F:", "+G:", "+H:"};
  for (uint8_t i = 0U; i < (uint8_t)(sizeof prefixes / sizeof prefixes[0]); ++i) {
    TEST_ASSERT_EQ(
      k_ra8_ok,
      ra8_modem_at_register_unsolicited_handler(prefixes[i], internal_urc_handler, nullptr));
  }
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_modem_at_register_unsolicited_handler("+I:", internal_urc_handler, nullptr));
  TEST_END("modem_at URC table full returns no_mem");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Exercise the @c internal_test_urc_invalid_prefix scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_urc_invalid_prefix(void)
{
  TEST_BEGIN("modem_at URC invalid prefix lengths");
  TEST_ASSERT_EQ(k_ra8_ok, internal_bring_up());
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_modem_at_register_unsolicited_handler("", internal_urc_handler, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_modem_at_register_unsolicited_handler("+X:", nullptr, nullptr));
  TEST_END("modem_at URC invalid prefix lengths");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Exercise the @c internal_test_poll_drains_urc scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_poll_drains_urc(void)
{
  TEST_BEGIN("modem_at poll drains URC outside command");
  TEST_ASSERT_EQ(k_ra8_ok, internal_bring_up());
  s_urc_hits = 0;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_modem_at_register_unsolicited_handler("+CREG:", internal_urc_handler, nullptr));
  internal_fifo_push_str(&s_io.modem_to_mcu, "\r\n+CREG: 0,1\r\n");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_poll());
  TEST_ASSERT_EQ(1, s_urc_hits);
  TEST_END("modem_at poll drains URC outside command");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Exercise the @c internal_test_expected_response scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_expected_response(void)
{
  TEST_BEGIN("modem_at expected_response prefix accepted");
  TEST_ASSERT_EQ(k_ra8_ok, internal_bring_up());
  internal_fifo_push_str(&s_io.modem_to_mcu, "AT+CPIN?\r\n\r\n+CPIN: READY\r\n\r\nOK\r\n");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd("AT+CPIN?", "+CPIN:", 1000U));
  TEST_END("modem_at expected_response prefix accepted");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Exercise the @c internal_test_send_cmd_null_arg scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_send_cmd_null_arg(void)
{
  TEST_BEGIN("modem_at send_cmd NULL arg");
  TEST_ASSERT_EQ(k_ra8_ok, internal_bring_up());
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_modem_at_send_cmd(nullptr, nullptr, 100U));
  TEST_END("modem_at send_cmd NULL arg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Exercise the @c internal_test_default_timeout_used scenario.
 * @details Drives the documented modem vectors through the in-memory transport
 * or private helper and checks every observable result.
 * @pre The test owns its fixture state.
 * @pre The test supplies every pointer and bound required by the exercised
 * path.
 * @post Every documented vector has been asserted.
 * @post No external resource remains owned by the test.
 * @note Runs synchronously in the host unit-test process.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_default_timeout_used(void)
{
  TEST_BEGIN("modem_at default timeout applied when 0 passed");
  TEST_ASSERT_EQ(k_ra8_ok, internal_bring_up());
  internal_fifo_push_str(&s_io.modem_to_mcu, "AT\r\n\r\nOK\r\n");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd("AT", nullptr, 0U));
  TEST_ASSERT(internal_mcu_tx_count() > 0U);
  TEST_END("modem_at default timeout applied when 0 passed");
}

int main(void)
{
  /* This test must run BEFORE internal_bring_up() so the initialized flag is 0.
   */
  internal_test_send_cmd_before_init_fails();

  internal_test_init_null_cfg();
  internal_test_init_short_buffer_rejected();
  internal_test_at_ok_with_echo();
  internal_test_at_error_returned();
  internal_test_at_cme_error();
  internal_test_timeout();
  internal_test_send_cmd_capture();
  internal_test_capture_buf_len_zero();
  internal_test_urc_dispatch();
  internal_test_urc_replace_same_prefix();
  internal_test_urc_table_full();
  internal_test_urc_invalid_prefix();
  internal_test_poll_drains_urc();
  internal_test_expected_response();
  internal_test_send_cmd_null_arg();
  internal_test_default_timeout_used();
  return 0;
}
