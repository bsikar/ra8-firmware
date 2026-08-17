/**
 * @file test_ra8_modem_at_mcdc_helpers.c
 * @brief Direct MC/DC vector tests for modem private helper contracts.
 *
 * @details
 * Owns the direct and pure helper vectors split from the transport-oriented
 * MC/DC suite. A minimal in-memory transport fixture initializes the driver
 * only for the classification/URC vector; the remaining cases invoke private
 * predicates directly through the sanctioned test-access header.
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
 * @enum t_at_buf_t
 * @brief Buffer capacities and the timeout the MC/DC vectors run under.
 */
typedef enum : uint16_t {
  k_t_capture_cap = 64U,   /**< Line-capture buffer, and the capacity handed to
                               the capture helper -- the two must agree or the
                               bounds vector tests the wrong limit.            */
  k_t_line_cap    = 256U,  /**< Line-assembly buffer, bytes. */
  k_t_timeout_ms  = 1000U, /**< Command timeout, long enough that no vector
                               expires by accident. */
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
 * @brief Reset one MC/DC fixture FIFO.
 * @details Restores both cursor indices without touching the backing bytes.
 * @param[in,out] f FIFO owned by the current test.
 * @pre @p f is non-NULL.
 * @pre No concurrent code accesses @p f.
 * @post The FIFO head is zero.
 * @post The FIFO tail is zero.
 * @note Backing bytes retain unspecified values.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_fifo_reset(test_fifo_t* f)
{
  f->head = 0U;
  f->tail = 0U;
}

/**
 * @brief Append one byte to an MC/DC fixture FIFO.
 * @details Stores and advances when capacity remains and otherwise preserves
 * the FIFO.
 * @param[in,out] f FIFO owned by the current test.
 * @param[in] b Byte to append.
 * @pre @p f is non-NULL.
 * @pre The FIFO indices describe its backing array.
 * @post A fitting byte is appended exactly once.
 * @post A full FIFO is unchanged.
 * @note Overflow is intentionally modeled as dropped input.
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
 * @brief Queue a NUL-terminated string as modem input.
 * @details Appends source bytes in order and excludes the terminating NUL.
 * @param[in,out] f FIFO owned by the current test.
 * @param[in] s NUL-terminated string to enqueue.
 * @pre @p f is non-NULL.
 * @pre @p s is non-NULL and NUL-terminated.
 * @post Each byte that fits has been offered in source order.
 * @post The source string is unchanged.
 * @note Capacity behavior comes from @ref internal_fifo_push.
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
 * @brief Pop one byte from an MC/DC fixture FIFO.
 * @details Writes the destination and advances only when data is available.
 * @param[in,out] f FIFO owned by the current test.
 * @param[out] out Destination for one available byte.
 * @return Zero on success, otherwise negative one.
 * @retval 0 One byte was returned.
 * @retval -1 The FIFO was empty.
 * @pre @p f is non-NULL.
 * @pre @p out is non-NULL.
 * @post Success advances the head once.
 * @post Empty input preserves the FIFO and destination.
 * @note The negative sentinel maps to a no-data modem error.
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
 * @brief Capture a transmitted byte in the MC/DC fixture.
 * @details Implements the modem transmit callback using the in-memory output
 * FIFO.
 * @param[in] ctx Unused transport context.
 * @param[in] byte Byte emitted by the driver.
 * @return The callback result.
 * @retval k_ra8_ok The fixture accepted the callback invocation.
 * @pre Fixture state has been reset.
 * @pre The transmit FIFO indices are valid.
 * @post The byte has been offered to the transmit FIFO.
 * @post No external I/O has occurred.
 * @note @p ctx is unused by this singleton fixture.
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
 * @brief Return one queued receive byte to the modem driver.
 * @details Converts the FIFO empty sentinel into the driver's no-data result.
 * @param[in] ctx Unused transport context.
 * @param[out] out Destination for one queued byte.
 * @return The callback result.
 * @retval k_ra8_ok A byte was returned.
 * @retval k_ra8_err_no_data The input FIFO was empty.
 * @pre @p out is non-NULL.
 * @pre The receive FIFO indices are valid.
 * @post Success advances the receive head once.
 * @post No-data preserves @p out and the FIFO.
 * @note @p ctx is unused by this singleton fixture.
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
 * @brief Advance and return the MC/DC fixture clock.
 * @details Adds the configured increment before exposing the fake timestamp.
 * @param[in] ctx Unused transport context.
 * @return Current fake time in milliseconds.
 * @retval 0 Time remains reset when automatic advance is zero.
 * @pre Fixture state has been initialized.
 * @pre No concurrent code mutates the clock.
 * @post Time advances by the configured increment modulo 32 bits.
 * @post No wall-clock source has been queried.
 * @note @p ctx is unused by this singleton fixture.
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
 * @brief Reset the MC/DC transport fixture.
 * @details Empties both FIFOs and restores the clock fields to zero.
 * @pre The test exclusively owns @ref s_io.
 * @pre Both FIFO objects have valid backing arrays.
 * @post Both FIFOs are empty.
 * @post Fake time and automatic advance are zero.
 * @note Modem initialization is performed separately.
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
 * @brief Initialize the modem against the MC/DC fixture.
 * @details Resets transport state and binds callbacks plus the static line
 * buffer.
 * @return The modem initialization result.
 * @retval k_ra8_ok The fixture configuration was accepted.
 * @pre The fixture owns @ref s_line_buf.
 * @pre No command is active.
 * @post Transport state is reset.
 * @post Success leaves the modem initialized with fixture callbacks.
 * @note Each dependent vector asserts this result.
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
    .default_timeout_ms = k_t_timeout_ms,
  };
  return ra8_modem_at_init(&cfg);
}

/* ------------------------------------------------------------------------- */
/* MC/DC vector tests for libs/ra8_modem_at/src/ra8_modem_at.c */
/* ------------------------------------------------------------------------- */

typedef enum : uint16_t {
  k_mcdc_capture_buf_bytes = 64U,   /**< Mcdc capture buffer bytes. */
  k_mcdc_prefix_too_big    = 64U,   /**< Mcdc prefix too big.       */
  k_mcdc_default_timeout   = 1000U, /**< Mcdc default timeout.      */
} modem_mcdc_caps_t;

/**
 * @brief Accept a URC without adding an observable side effect.
 * @details Provides a valid handler for vectors concerned only with
 * registration and dispatch.
 * @param[in] line NUL-terminated unsolicited response line.
 * @param[in] ctx Unused callback context.
 * @pre @p line is non-NULL and NUL-terminated.
 * @pre The callback is invoked synchronously by the fixture.
 * @post The input line is unchanged.
 * @post No fixture state is modified.
 * @note Both parameters are intentionally ignored.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_mcdc_dummy_urc(const char* line, void* ctx)
{
  (void)line;
  (void)ctx;
}

/**
 * @test internal_test_mcdc_internal_classify_cmd_echo_pair
 *
 * @par MC/DC:
 * Decision at libs/ra8_modem_at/src/ra8_modem_at.c
 *   ``(cmd_echo != nullptr) && (priv_modem_str_eq(line, cmd_echo) != 0U)``
 * (2 conditions, AND). N+1 = 3 vectors. Driven directly against
 * production source via ra8_modem_at_internal.h (test-access policy,
 * see CLAUDE.md "Test access to internal symbols").
 *
 * - V1 cmd_echo=NULL                  -> C1=F shorts.       Decision F.
 * - V2 cmd_echo="AT", line="AT"       -> C1=T, C2=T.        Decision T (echo).
 * - V3 cmd_echo="AT", line="OTHER"    -> C1=T, C2=F.        Decision F.
 * V1+V2 isolate C1; V2+V3 isolate C2.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 * @brief Exercise the @c internal_test_mcdc_internal_classify_cmd_echo_pair
 * scenario.
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
static void internal_test_mcdc_internal_classify_cmd_echo_pair(void)
{
  TEST_BEGIN("modem_at MC/DC: cmd_echo AND (priv_modem_classify)");
  /* V1: cmd_echo=NULL -> short circuit -> not classified as echo. */
  TEST_ASSERT(priv_modem_classify("AT", nullptr, nullptr) != k_ra8_modem_line_kind_echo);
  /* V2: both true -> echo. */
  TEST_ASSERT_EQ(k_ra8_modem_line_kind_echo, priv_modem_classify("AT", "AT", nullptr));
  /* V3: cmd_echo non-NULL but line mismatch -> not echo. */
  TEST_ASSERT(priv_modem_classify("OTHER", "AT", nullptr) != k_ra8_modem_line_kind_echo);
  TEST_END("modem_at MC/DC: cmd_echo AND (priv_modem_classify)");
}

/**
 * @test internal_test_mcdc_internal_str_len_pair
 *
 * @par MC/DC:
 * Decision at libs/ra8_modem_at/src/ra8_modem_at.c
 *   ``while ((i < UINT16_MAX) && (s[i] != '\0'))``
 *
 * - V1: s = "AB" -> at i=0 C1=T, C2=T (loop runs and exits via C2 at i=2).
 * - V2: s = ""   -> at i=0 C1=T, C2=F (loop exits via C2 immediately).
 *   Pair (V1,V2) isolates C2 with C1 held at T.
 * - V3: a 65535-byte buffer of non-zero bytes (no embedded NUL until
 *   index 65535) drives ``i`` to ``UINT16_MAX`` so C1 evaluates F at
 *   the loop-exit step. Pair (V1,V3) isolates C1 with C2 held at T.
 *
 * Three vectors = N+1 minimal MC/DC for N=2.
 * @brief Exercise the @c internal_test_mcdc_internal_str_len_pair scenario.
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
static void internal_test_mcdc_internal_str_len_pair(void)
{
  TEST_BEGIN("modem_at MC/DC: internal_str_len short-circuit");
  TEST_ASSERT_EQ(0, priv_modem_str_len(""));
  TEST_ASSERT_EQ(2, priv_modem_str_len("AB"));
  TEST_ASSERT_EQ(5, priv_modem_str_len("HELLO"));

  /* V3: drive i to UINT16_MAX so C1 ("i < UINT16_MAX") flips to F. */
  static char s_huge_text[(size_t)UINT16_MAX + 1U];
  for (size_t k = 0U; k < (size_t)UINT16_MAX; ++k) {
    s_huge_text[k] = 'x';
  }
  s_huge_text[(size_t)UINT16_MAX] = '\0';
  TEST_ASSERT_EQ(UINT16_MAX, priv_modem_str_len(s_huge_text));

  TEST_END("modem_at MC/DC: internal_str_len short-circuit");
}

/**
 * @test internal_test_mcdc_internal_str_eq_loop_pair
 *
 * @par MC/DC:
 * Decision at libs/ra8_modem_at/src/ra8_modem_at.c
 *   ``while ((a[i] != '\0') && (b[i] != '\0'))``
 *
 * - V1: a="X",  b="X"   -> i=0 C1=T C2=T (enter), i=1 C1=F (exit via C1).
 * - V2: a="",   b="Y"   -> i=0 C1=F (exit via C1, C2 not evaluated -> false).
 *   Pair (V1@i=0, V2) isolates C1 with C2=T held.
 * - V3: a="X",  b=""    -> i=0 C1=T C2=F (exit via C2). Pair (V1@i=0, V3)
 *   isolates C2 with C1=T held.
 *
 * N=2 -> N+1=3 vectors. Minimal MC/DC.
 * @brief Exercise the @c internal_test_mcdc_internal_str_eq_loop_pair scenario.
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
static void internal_test_mcdc_internal_str_eq_loop_pair(void)
{
  TEST_BEGIN("modem_at MC/DC: internal_str_eq loop short-circuit");
  TEST_ASSERT_EQ(1, priv_modem_str_eq("X", "X"));
  TEST_ASSERT_EQ(0, priv_modem_str_eq("", "Y"));
  TEST_ASSERT_EQ(0, priv_modem_str_eq("X", ""));
  TEST_ASSERT_EQ(1, priv_modem_str_eq("", ""));
  TEST_ASSERT_EQ(0, priv_modem_str_eq("AB", "AC"));
  TEST_END("modem_at MC/DC: internal_str_eq loop short-circuit");
}

/**
 * @test internal_test_mcdc_internal_str_eq_terminator_pair
 *
 * @par MC/DC:
 * Decision at libs/ra8_modem_at/src/ra8_modem_at.c
 *   ``return (uint8_t)((a[i] == '\0') && (b[i] == '\0'));``
 *
 * After the loop terminates, the terminator-AND is evaluated on the
 * common index i. Vectors:
 * - V1: a="",  b=""   -> C1=T C2=T -> 1 (equal empties).
 * - V2: a="X", b=""   -> loop stops at i=0 with a[0]='X', b[0]='\0';
 *   C1=F (exit via C1) -> 0.
 * - V3: a="",  b="X"  -> C1=T C2=F -> 0.
 * Pair (V1,V2) isolates C1; (V1,V3) isolates C2. N=2 -> 3 vectors.
 * @brief Exercise the @c internal_test_mcdc_internal_str_eq_terminator_pair
 * scenario.
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
static void internal_test_mcdc_internal_str_eq_terminator_pair(void)
{
  TEST_BEGIN("modem_at MC/DC: internal_str_eq terminator AND");
  TEST_ASSERT_EQ(1, priv_modem_str_eq("", ""));
  TEST_ASSERT_EQ(0, priv_modem_str_eq("X", ""));
  TEST_ASSERT_EQ(0, priv_modem_str_eq("", "X"));
  TEST_ASSERT_EQ(1, priv_modem_str_eq("AB", "AB"));
  TEST_ASSERT_EQ(0, priv_modem_str_eq("AB", "ABC"));
  TEST_END("modem_at MC/DC: internal_str_eq terminator AND");
}

/**
 * @test internal_test_mcdc_internal_starts_with
 *
 * @par MC/DC:
 * Auxiliary direct-call coverage for the helper used in the
 * priv_modem_classify line-352 OR-chain. While the classify
 * test exercises the chain at the call site, hitting starts_with
 * directly forces the inner ``if (hay[i] != needle[i])`` branch on the
 * production source.
 *
 * Vectors:
 * - V1: hay="ABC", needle="AB" -> match (returns 1).
 * - V2: hay="ABC", needle="AX" -> mismatch in body (returns 0).
 * - V3: hay="A",   needle=""   -> empty needle short-circuits (returns 1).
 *
 * Single-condition decisions (`needle[i]!='\0'`, `hay[i]!=needle[i]`)
 * each get T and F vectors. No compound decision in this helper, so
 * MC/DC reduces to branch coverage; 3 vectors confirm both branches.
 * @brief Exercise the @c internal_test_mcdc_internal_starts_with scenario.
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
static void internal_test_mcdc_internal_starts_with(void)
{
  TEST_BEGIN("modem_at MC/DC: internal_starts_with branches");
  TEST_ASSERT_EQ(1, priv_modem_starts_with("ABC", "AB"));
  TEST_ASSERT_EQ(0, priv_modem_starts_with("ABC", "AX"));
  TEST_ASSERT_EQ(1, priv_modem_starts_with("A", ""));
  TEST_END("modem_at MC/DC: internal_starts_with branches");
}

/**
 * @test internal_test_mcdc_internal_capture_line_guard
 *
 * @par MC/DC:
 * Decision at libs/ra8_modem_at/src/ra8_modem_at.c
 *   ``if ((capture == nullptr) || (capture_len == 0U))`` (2 conditions, OR)
 *
 * - V1: capture=NULL,  capture_len=10 -> C1=T short-circuit, returns early.
 * - V2: capture=valid, capture_len=0  -> C1=F C2=T,         returns early.
 * - V3: capture=valid, capture_len=64 -> C1=F C2=F,         performs append.
 * V1+V3 isolate C1; V2+V3 isolate C2. N+1 = 3 vectors: minimal MC/DC.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 * @brief Exercise the @c internal_test_mcdc_internal_capture_line_guard
 * scenario.
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
static void internal_test_mcdc_internal_capture_line_guard(void)
{
  TEST_BEGIN("modem_at MC/DC: internal_capture_line guard OR");
  char   buf[k_t_capture_cap] = {};
  size_t used                 = 0U;

  /* V1: capture=NULL -> short-circuit (no crash, used unchanged). */
  priv_modem_capture_line("hi", nullptr, k_t_capture_cap, &used);
  TEST_ASSERT_EQ(0, used);

  /* V2: capture valid but capture_len==0 -> early return. */
  priv_modem_capture_line("hi", buf, 0U, &used);
  TEST_ASSERT_EQ(0, used);

  /* V3: both false -> append succeeds. */
  priv_modem_capture_line("hi", buf, sizeof(buf), &used);
  TEST_ASSERT(used >= 2U);
  TEST_ASSERT_EQ('h', buf[0]);
  TEST_ASSERT_EQ('i', buf[1]);

  TEST_END("modem_at MC/DC: internal_capture_line guard OR");
}

/**
 * @test internal_test_mcdc_reset_line_should_clear_pure
 *
 * @par MC/DC:
 * Decision (pure helper): ``(line_buf != nullptr) && (line_buf_len > 0U)``
 * (2 conditions, AND; N+1 = 3 vectors). The decision was promoted out
 * of @c internal_reset_line into the pure sibling
 * @c priv_modem_reset_line_should_clear so all four input
 * combinations are reachable from a host test (the production wrapper
 * is gated by the init validator -- see
 * @c internal_test_mcdc_reset_line_buf_pair for the validator-equivalence
 * argument). Maps directly to libs/ra8_modem_at/src/ra8_modem_at.c
 * and the promoted-helper site at libs/ra8_modem_at/src/ra8_modem_at.c.
 *
 * - V1: buf=NULL,    len=8 -> C1=F shorts.        Decision F (no clear).
 * - V2: buf=valid,   len=0 -> C1=T C2=F.          Decision F (no clear).
 * - V3: buf=valid,   len=8 -> all T.              Decision T (clear).
 * V1+V3 isolate C1; V2+V3 isolate C2.
 * @brief Exercise the @c internal_test_mcdc_reset_line_should_clear_pure
 * scenario.
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
static void internal_test_mcdc_reset_line_should_clear_pure(void)
{
  TEST_BEGIN("modem_at MC/DC: reset_line_should_clear (pure)");
  uint8_t scratch[8] = {0U};
  TEST_ASSERT_EQ(0, priv_modem_reset_line_should_clear(nullptr, 8U));
  TEST_ASSERT_EQ(0, priv_modem_reset_line_should_clear(scratch, 0U));
  TEST_ASSERT_EQ(1, priv_modem_reset_line_should_clear(scratch, 8U));
  TEST_END("modem_at MC/DC: reset_line_should_clear (pure)");
}

/**
 * @test internal_test_mcdc_internal_classify_expected_direct
 *
 * @par MC/DC:
 * Decision at libs/ra8_modem_at/src/ra8_modem_at.c
 *   ``(expected_response == nullptr) || (expected_response[0] == '\0') ||
 *    (priv_modem_starts_with(line, expected_response) == 0U)``
 * (3 conditions, OR; N+1 = 4 vectors).
 *
 * The existing @c internal_test_mcdc_internal_classify_expected drives this
 * through @c ra8_modem_at_send_cmd_capture; the indirect path masks
 * which condition actually flips the URC-allowed result. This test
 * calls @c priv_modem_classify directly so the production
 * decision sees four vectors with one condition flipping per pair.
 *
 * - V1: expected=NULL,         line="OTHER" -> C1=T short.        URC-allowed
 * -> kind=payload (no URC reg).
 * - V2: expected="",           line="OTHER" -> C1=F C2=T.         URC-allowed
 * -> kind=payload.
 * - V3: expected="+QRY",       line="OTHER" -> C1=F C2=F C3=T.    URC-allowed
 * -> kind=payload.
 * - V4: expected="+QRY",       line="+QRY:val" -> C1=F C2=F C3=F.
 * URC-suppressed -> kind=payload (no URC dispatch).
 *
 * V1 vs V4 isolate C1; V2 vs V4 isolate C2; V3 vs V4 isolate C3. The
 * observable side-effect of "URC dispatch allowed" is whether the
 * registered URC handler counter is incremented when the line matches
 * a registered URC prefix.
 */
static uint32_t s_mcdc_classify_urc_calls = 0U;

/**
 * @brief Count classification-time URC dispatches.
 * @details Provides the observable side effect for direct expected-prefix
 * classification vectors.
 * @param[in] line NUL-terminated unsolicited response line.
 * @param[in] ctx Unused callback context.
 * @pre @p line is non-NULL and NUL-terminated.
 * @pre The fixture exclusively owns the counter.
 * @post The dispatch counter has advanced by one.
 * @post The input line and context are unchanged.
 * @note Both callback arguments are intentionally ignored.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_mcdc_count_urc_handler(const char* line, void* ctx)
{
  (void)line;
  (void)ctx;
  s_mcdc_classify_urc_calls++;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 * @brief Exercise the @c internal_test_mcdc_internal_classify_expected_direct
 * scenario.
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
static void internal_test_mcdc_internal_classify_expected_direct(void)
{
  TEST_BEGIN("modem_at MC/DC: classify expected_response (3-cond OR, direct)");
  TEST_ASSERT_EQ(k_ra8_ok, internal_bring_up());
  /* Register a URC handler so we can observe URC dispatch. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_modem_at_register_unsolicited_handler("+QRY", internal_mcdc_count_urc_handler, nullptr));
  s_mcdc_classify_urc_calls = 0U;

  /* V1: expected=NULL, line "+QRY:async" -> URC dispatch allowed
   * (C1=T) and registered handler matches -> URC fires. */
  ra8_modem_line_kind_t k1 = priv_modem_classify("+QRY:async", nullptr, nullptr);
  TEST_ASSERT_EQ(k_ra8_modem_line_kind_urc, k1);
  TEST_ASSERT_EQ(1, s_mcdc_classify_urc_calls);

  /* V2: expected="", line "+QRY:async" -> C1=F C2=T -> URC allowed -> URC
   * fires. */
  ra8_modem_line_kind_t k2 = priv_modem_classify("+QRY:async", nullptr, "");
  TEST_ASSERT_EQ(k_ra8_modem_line_kind_urc, k2);
  TEST_ASSERT_EQ(2, s_mcdc_classify_urc_calls);

  /* V3: expected="+OTH", line "+QRY:async" -> C1=F C2=F C3=T (no
   * starts_with match) -> URC allowed -> URC fires. */
  ra8_modem_line_kind_t k3 = priv_modem_classify("+QRY:async", nullptr, "+OTH");
  TEST_ASSERT_EQ(k_ra8_modem_line_kind_urc, k3);
  TEST_ASSERT_EQ(3, s_mcdc_classify_urc_calls);

  /* V4: expected="+QRY", line "+QRY:val" -> all conds F -> URC
   * dispatch suppressed -> URC handler NOT fired -> kind=payload. */
  ra8_modem_line_kind_t k4 = priv_modem_classify("+QRY:val", nullptr, "+QRY");
  TEST_ASSERT_EQ(k_ra8_modem_line_kind_payload, k4);
  TEST_ASSERT_EQ(3, s_mcdc_classify_urc_calls);

  TEST_END("modem_at MC/DC: classify expected_response (3-cond OR, direct)");
}

/**
 * @test internal_test_mcdc_handle_line_payload_prefix_match_pure
 *
 * @par MC/DC:
 * Decision (ra8_modem_at.c line 573, internal_handle_line payload case,
 * carried by priv_modem_payload_prefix_matches):
 *   `(expected_response != nullptr) && (expected_response[0] != '\0') &&
 *    (priv_modem_starts_with(line, expected_response) != 0U)`
 * (3 conditions, AND short-circuit). N+1 = 4 vectors.
 *
 * - V1: expected=NULL, line="+QRY:val"        -> C1=F. result=0 (short-circ).
 * - V2: expected="",   line="+QRY:val"        -> C1=T,C2=F. result=0.
 * - V3: expected="+QRY", line="OTHER"         -> C1=T,C2=T,C3=F. result=0.
 * - V4: expected="+QRY", line="+QRY:val"      -> C1=T,C2=T,C3=T. result=1.
 * V1 vs V4 isolate C1 (only C1 differs between them); V2 vs V4 isolate C2;
 * V3 vs V4 isolate C3. N+1 = 4 vectors: minimal MC/DC.
 * @brief Exercise the @c
 * internal_test_mcdc_handle_line_payload_prefix_match_pure scenario.
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
static void internal_test_mcdc_handle_line_payload_prefix_match_pure(void)
{
  TEST_BEGIN("modem_at MC/DC: handle_line payload prefix match (3-cond AND, direct)");
  /* V1 */
  TEST_ASSERT_EQ(0, priv_modem_payload_prefix_matches("+QRY:val", nullptr));
  /* V2 */
  TEST_ASSERT_EQ(0, priv_modem_payload_prefix_matches("+QRY:val", ""));
  /* V3 */
  TEST_ASSERT_EQ(0, priv_modem_payload_prefix_matches("OTHER", "+QRY"));
  /* V4 */
  TEST_ASSERT_EQ(1, priv_modem_payload_prefix_matches("+QRY:val", "+QRY"));
  TEST_END("modem_at MC/DC: handle_line payload prefix match (3-cond AND, direct)");
}

/**
 * @test internal_test_mcdc_wait_response_should_clear_capture_pure
 *
 * @par MC/DC:
 * Decision (ra8_modem_at.c line 664, internal_wait_response capture-init,
 * carried by priv_modem_capture_should_clear):
 *   `(capture != nullptr) && (capture_len > 0U)`  (2 conditions, AND).
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 *
 * - V1: capture=NULL, capture_len=0  -> C1=F. result=0.
 * - V2: capture=NULL, capture_len=8  -> C1=F (short-circ). result=0.
 *       (V1 vs V2 cannot isolate either condition; V2 added so C2 is
 *       proven independently against V3.)
 * - V3: capture=buf,  capture_len=0  -> C1=T,C2=F. result=0.
 * - V4: capture=buf,  capture_len=8  -> C1=T,C2=T. result=1.
 * V1+V4 vary C1 (and C2 -- so use V3 vs V4 to isolate C1 cleanly with C2
 * held T). V3+V4 isolate C2 (C1 held T). V1+V3 isolate C1 (C2 held F).
 * Minimal MC/DC for 2-cond AND = 3 vectors {V1, V3, V4}; V2 added as a
 * regression sentinel for the structurally-impossible-via-public-API
 * (NULL, >0) input.
 * @brief Exercise the @c
 * internal_test_mcdc_wait_response_should_clear_capture_pure scenario.
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
static void internal_test_mcdc_wait_response_should_clear_capture_pure(void)
{
  TEST_BEGIN("modem_at MC/DC: wait_response should-clear capture (2-cond AND, "
             "direct)");
  char buf[8];
  /* V1: F,F */
  TEST_ASSERT_EQ(0, priv_modem_capture_should_clear(nullptr, 0U));
  /* V2: F,T (short-circuited at C1) */
  TEST_ASSERT_EQ(0, priv_modem_capture_should_clear(nullptr, 8U));
  /* V3: T,F */
  TEST_ASSERT_EQ(0, priv_modem_capture_should_clear(buf, 0U));
  /* V4: T,T */
  TEST_ASSERT_EQ(1, priv_modem_capture_should_clear(buf, 8U));
  TEST_END("modem_at MC/DC: wait_response should-clear capture (2-cond AND, "
           "direct)");
}
int main(void)
{
  internal_test_mcdc_internal_classify_cmd_echo_pair();
  internal_test_mcdc_internal_str_len_pair();
  internal_test_mcdc_internal_str_eq_loop_pair();
  internal_test_mcdc_internal_str_eq_terminator_pair();
  internal_test_mcdc_internal_starts_with();
  internal_test_mcdc_internal_capture_line_guard();
  internal_test_mcdc_reset_line_should_clear_pure();
  internal_test_mcdc_internal_classify_expected_direct();
  internal_test_mcdc_handle_line_payload_prefix_match_pure();
  internal_test_mcdc_wait_response_should_clear_capture_pure();
  return 0;
}
