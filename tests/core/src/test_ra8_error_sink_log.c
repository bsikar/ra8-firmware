/**
 * @file test_ra8_error_sink_log.c
 * @brief Unit tests for the production `ra8_error_interface_t`
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 *
 * @details
 * `g_ra8_error_sink_log` is the injectable non-fatal error sink
 * `ra8_error_interface.h` publishes: the default a driver binds so it
 * can report a degraded sensor or a CRC mismatch without halting
 * through `ra8_fatal_error()`. These cases pin what a consumer relies
 * on when it binds that sink from a file-scope initializer:
 *
 *  1. the object links, `report` is callable without an init step, and
 *     `ctx` is NULL because the sink holds no state,
 *  2. a report reaches the log backend rendered as an ERROR line whose
 *     companion value is the `ra8_err_t` code,
 *  3. a NULL `tag` or `msg` is substituted rather than handed to the
 *     formatter, and the report still goes out,
 *  4. with no backend sink installed the report is silently dropped
 *     instead of faulting: reporting is best-effort by contract.
 *
 * Observability: `ra8_log_set_byte_sink` installs a capturing byte
 * sink on the hosted backend, so each case asserts the exact rendered
 * line. That is stricter than watching the call return, and it is what
 * makes case 3 provable: the substituted strings appear in the bytes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

/* Bind the log macros to the real emitters rather than the no-op form. */
/** @brief RA8 LOG LEVEL. */
#define RA8_LOG_LEVEL k_ra8_log_level_debug

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_error_interface.h"
#include "ra8_fake_mmap.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/**
 * @enum error_sink_cap_t
 * @brief Capacity of the capturing byte sink these cases install.
 */
typedef enum : uint16_t {
  k_error_sink_cap = 128U, /**< Capture buffer size in bytes, NUL included. */
} error_sink_cap_t;

/**
 * @struct error_sink_capture_t
 * @brief Bytes the log backend produced while one report was forwarded.
 *
 * @details The backend hands the sink one byte at a time, so a whole line can
 *          only be asserted by accumulating it. `dropped` exists so a line
 *          that outgrew the buffer fails loudly instead of comparing equal to
 *          its own truncation.
 *
 * @invariant `text` is NUL-terminated at index `len`.
 * @invariant `dropped` is 0 whenever the captured line fitted.
 *
 * @since 0.1.0
 */
typedef struct {
  char     text[k_error_sink_cap]; /**< Bytes the logger handed to the sink.  */
  uint32_t len;                    /**< Number of bytes captured in `text`.   */
  uint32_t dropped;                /**< Bytes refused after `text` filled up. */
} error_sink_capture_t;

/**
 * @var s_capture
 * @brief Capture buffer the byte sink writes into.
 * @details Holds the exact line the backend rendered for one report, so the
 *          assertions compare bytes rather than merely observing a return.
 * @note Single-threaded fixture; no synchronization is provided.
 * @warning Reset it with ::internal_capture_reset before every report.
 * @since 0.1.0
 */
static error_sink_capture_t s_capture = {};

/**
 * @brief Append one log byte to ::s_capture.
 *
 * @param[in,out] ctx  Capture cookie (always &::s_capture).
 * @param[in]     byte The log byte the backend produced.
 *
 * @pre @p ctx addresses one writable ::error_sink_capture_t.
 * @post The byte is appended, or `dropped` has been incremented.
 * @post `text` remains NUL-terminated.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_capture_byte(void* ctx, uint8_t byte)
{
  error_sink_capture_t* cap = (error_sink_capture_t*)ctx;
  if (cap->len >= ((uint32_t)k_error_sink_cap - 1U)) {
    cap->dropped++;
    return;
  }
  cap->text[cap->len] = (char)byte;
  cap->len++;
  cap->text[cap->len] = '\0';
}

/**
 * @brief Empty ::s_capture before the next report is forwarded.
 *
 * @pre The previous case's assertions have already run.
 * @post `len` and `dropped` are zero and `text` is empty.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_capture_reset(void)
{
  s_capture = (error_sink_capture_t){};
}

/**
 * @brief Require ::s_capture to hold exactly @p expected.
 *
 * @details Compares length as well as bytes, so a line that merely starts
 *          with @p expected, or one the buffer truncated, fails.
 *
 * @param[in] expected The complete line the backend should have rendered.
 *
 * @pre @p expected is NUL-terminated and one report was forwarded since the
 *      last reset.
 * @post No fixture state is modified.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_capture(const char* expected)
{
  TEST_ASSERT_EQ(0U, s_capture.dropped);
  TEST_ASSERT_EQ(strlen(expected), s_capture.len);
  TEST_ASSERT(strcmp(s_capture.text, expected) == 0);
}

/**
 * @brief Install the capturing sink on a freshly initialised backend.
 *
 * @pre The fake MMIO backend is available.
 * @post The hosted backend reports ready and writes into ::s_capture.
 *
 * @note Not thread-safe; the fake MMIO page is process-global.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bind_capture(void)
{
  ra8_fake_mmap_reset();
  ra8_log_set_byte_sink(internal_capture_byte, &s_capture);
  ra8_log_init();
}

/**
 * @brief The published sink object is bound and stateless.
 *
 * @details Asserts `report` is non-NULL, which is the whole point of the
 *          symbol (a consumer calls through it with no init step), and that
 *          `ctx` is NULL, which is what lets a driver copy the struct and
 *          overwrite `ctx` with its own without losing sink state.
 *
 * @pre None.
 * @post No state modified.
 *
 * @note Trivially thread-safe: reads a const object.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- asserts the published object's
 * field contract; no `&&` or `||` in the code under test that this case
 * touches)
 */
RA8_INTERNAL static void internal_test_sink_object_bound(void)
{
  TEST_BEGIN("g_ra8_error_sink_log is bound and stateless");

  TEST_ASSERT_NOT_NULL(g_ra8_error_sink_log.report);
  TEST_ASSERT_NULL(g_ra8_error_sink_log.ctx);

  TEST_END("g_ra8_error_sink_log is bound and stateless");
}

/**
 * @brief A report with a real tag and message renders an ERROR line.
 *
 * @details `k_ra8_err_timeout` is 0x108, so the companion value must render
 *          as decimal 264. Asserting the value as well as the strings is what
 *          proves the sink forwards the error code rather than a placeholder.
 *
 * @pre The fake MMIO backend is available.
 * @post One rendered line sits in ::s_capture; no caller state modified.
 *
 * @note Not thread-safe; shares the process-global fake MMIO page.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (exercises the true side of both `tag != NULL` and `msg != NULL` in
 * `internal_error_sink_log_report`; the false sides are covered by the
 * NULL-substitution case below)
 */
RA8_INTERNAL static void internal_test_sink_forwards_report(void)
{
  TEST_BEGIN("sink renders a report as an ERROR line");

  internal_bind_capture();
  internal_capture_reset();

  g_ra8_error_sink_log.report(g_ra8_error_sink_log.ctx, "UNIT", "degraded sensor",
                              k_ra8_err_timeout);
  internal_expect_capture("[UNIT] ERROR: degraded sensor=264\r\n");

  TEST_END("sink renders a report as an ERROR line");
}

/**
 * @brief NULL tag and message are substituted, not dereferenced.
 *
 * @details Reports with both string arguments NULL. The house strings must
 *          appear in the rendered bytes, which separates substitution from an
 *          early return that silently drops the report. `k_ra8_err_null_ptr`
 *          is 0x504, so the value renders as decimal 1284.
 *
 * @pre The fake MMIO backend is available.
 * @post One rendered line sits in ::s_capture; no caller state modified.
 *
 * @note Not thread-safe; shares the process-global fake MMIO page.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (exercises the false side of both `tag != NULL` and `msg != NULL`,
 * completing the pair with the forwarding case above)
 */
RA8_INTERNAL static void internal_test_sink_null_substitution(void)
{
  TEST_BEGIN("sink substitutes a NULL tag and message");

  internal_bind_capture();
  internal_capture_reset();

  g_ra8_error_sink_log.report(NULL, NULL, NULL, k_ra8_err_null_ptr);
  internal_expect_capture("[ERR_SINK] ERROR: (no message)=1284\r\n");

  TEST_END("sink substitutes a NULL tag and message");
}

/**
 * @brief Only one of the two strings NULL still substitutes that one.
 *
 * @details Pins the two guards as independent: a real tag with a NULL message
 *          must keep the tag and substitute only the message. A single guard
 *          covering both arguments would fail here.
 *
 * @pre The fake MMIO backend is available.
 * @post One rendered line sits in ::s_capture; no caller state modified.
 *
 * @note Not thread-safe; shares the process-global fake MMIO page.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (holds `tag != NULL` true while `msg != NULL` is false, so neither
 * guard can be removed without this case failing)
 */
RA8_INTERNAL static void internal_test_sink_null_message_only(void)
{
  TEST_BEGIN("sink substitutes only the NULL argument");

  internal_bind_capture();
  internal_capture_reset();

  g_ra8_error_sink_log.report(NULL, "RETRY", NULL, k_ra8_err_timeout);
  internal_expect_capture("[RETRY] ERROR: (no message)=264\r\n");

  TEST_END("sink substitutes only the NULL argument");
}

/**
 * @brief With no backend sink installed the report is dropped, not fatal.
 *
 * @details A non-fatal report is best-effort: a driver reporting a degraded
 *          sensor before the log backend has a sink must not fault or block.
 *
 * @pre The fake MMIO backend is available.
 * @post Nothing was captured; no caller state modified.
 *
 * @note Not thread-safe; shares the process-global fake MMIO page.
 *
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the backend's
 * no-sink early return through the sink)
 */
RA8_INTERNAL static void internal_test_sink_without_backend(void)
{
  TEST_BEGIN("sink drops a report when no backend sink is installed");

  internal_bind_capture();
  ra8_log_set_byte_sink(nullptr, nullptr);
  internal_capture_reset();

  g_ra8_error_sink_log.report(NULL, "UNIT", "crc mismatch", k_ra8_err_crc_mismatch);

  TEST_ASSERT_EQ(0U, s_capture.len);
  TEST_ASSERT_EQ(0U, s_capture.dropped);

  TEST_END("sink drops a report when no backend sink is installed");
}

/**
 * @brief Run every error-sink case.
 *
 * @return 0 on success; a failing case exits the process itself.
 *
 * @since 0.1.0
 */
int main(void)
{
  internal_test_sink_object_bound();
  internal_test_sink_forwards_report();
  internal_test_sink_null_substitution();
  internal_test_sink_null_message_only();
  internal_test_sink_without_backend();
  return 0;
}
