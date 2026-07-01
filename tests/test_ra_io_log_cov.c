/**
 * @file test_ra_io_log_cov.c
 * @brief Coverage top-up for the ra_log -> ra_io stream adapter (ra_io_log.c).
 *
 * @details
 * The end-to-end redirect is already exercised by test_ra_io_stream.c. This
 * file drives the two remaining branches that suite never reached:
 *   - ::ra_io_log_attach rejecting a bound-but-uninitialised stream handle
 *     (`s->iface == nullptr`) with k_ra_err_not_initialized, and
 *   - the happy-path attach/emit/detach cycle, so the byte-sink write leg and
 *     the detach reset run inside this executable as well.
 * Coverage is merged across every test binary by gcovr, so exercising these
 * legs here fills the ra_io_log.c gap without touching the production adapter.
 *
 * @note The static byte sink's `ctx == nullptr` guard is undrivable through the
 * linked public API (the adapter only ever installs the sink with the non-NULL
 * stream validated by ::ra_io_log_attach), so it carries a GCOVR_EXCL_LINE
 * marker at its source rather than a fabricated test.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_io_log.h"
#include "ra_io_stream.h"
#include "ra_io_stream_ram.h"
#include "ra_log.h"
#include "unity_minimal.h"

/**
 * @enum t_io_log_const_t
 * @brief Fixture sizes for the log-capture RAM sink.
 */
typedef enum : uint32_t {
  k_t_log_cap = 128, /**< RAM sink capacity for the captured log line. */
} t_io_log_const_t;

/**
 * @brief Attach rejects NULL and an uninitialised (unbound) stream handle.
 *
 * @details
 * Covers both guards in ::ra_io_log_attach: the RA_CHECK_NULL_PTR pointer guard
 * and the `s->iface == nullptr` not-initialised guard reached with a
 * zero-initialised handle that was never bound to a sink.
 *
 * @par MC/DC:
 * Two independent single-condition guards, tested one vector each:
 * - `s == nullptr`      -> k_ra_err_null_ptr        (pointer guard true).
 * - `s->iface == nullptr` -> k_ra_err_not_initialized (iface guard true).
 * The valid-bound case in ::test_attach_emit_detach supplies the false vector
 * for both guards (returns k_ra_ok), completing independent influence.
 */
static void test_attach_rejects_invalid(void)
{
  TEST_BEGIN("attach rejects invalid");
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_io_log_attach(nullptr));

  ra_io_stream_t unbound = {}; /* iface == nullptr: never bound to a sink. */
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_io_log_attach(&unbound));
  TEST_END("attach rejects invalid");
}

/**
 * @brief Attach a bound stream, emit a log line, then detach.
 *
 * @details
 * Drives the false leg of both ::ra_io_log_attach guards (a valid bound RAM
 * stream returns k_ra_ok), routes a real ra_log line through the installed byte
 * sink so the per-byte stream write runs, and restores the default backend via
 * ::ra_io_log_detach. Confirms the tag and message landed in the capture buffer.
 *
 * @par MC/DC:
 * (no compound decisions under test -- a valid bound handle is accepted, the
 * captured buffer is inspected, and detach clears the sink)
 */
static void test_attach_emit_detach(void)
{
  TEST_BEGIN("attach emit detach");
  static char              buf[(size_t)k_t_log_cap] = {};
  ra_io_stream_ram_state_t st                       = {};
  ra_io_stream_t           s                        = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_io_stream_ram_init(&s, &st, (uint8_t*)buf, k_t_log_cap - 1u));

  TEST_ASSERT_EQ(k_ra_ok, ra_io_log_attach(&s));
  ra_log_init();
  ra_log_error("IOL", "line");
  ra_io_log_detach();

  uint32_t used = 0;
  TEST_ASSERT_EQ(k_ra_ok, ra_io_stream_ram_used(&st, &used));
  TEST_ASSERT(used > 0u);
  buf[used] = '\0';
  TEST_ASSERT(strstr(buf, "IOL") != nullptr);
  TEST_ASSERT(strstr(buf, "line") != nullptr);
  TEST_END("attach emit detach");
}

int32_t main(void)
{
  test_attach_rejects_invalid();
  test_attach_emit_detach();
  (void)fprintf(stderr, "[OK  ] test_ra_io_log_cov.c\n");
  return 0;
}
