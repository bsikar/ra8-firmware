/**
 * @file test_ra8_io_log_cov.c
 * @brief Coverage top-up for the ra8_log -> ra8_io stream adapter (ra8_io_log.c).
 *
 * @details
 * The end-to-end redirect is already exercised by test_ra8_io_stream.c. This
 * file drives the two remaining branches that suite never reached:
 *   - ::ra8_io_log_attach rejecting a bound-but-uninitialised stream handle
 *     (`s->iface == nullptr`) with k_ra8_err_not_initialized, and
 *   - the happy-path attach/emit/detach cycle, so the byte-sink write leg and
 *     the detach reset run inside this executable as well.
 * Coverage is merged across every test binary by gcovr, so exercising these
 * legs here fills the ra8_io_log.c gap without touching the production adapter.
 *
 * @note The static byte sink's `ctx == nullptr` guard is undrivable through the
 * linked public API (the adapter only ever installs the sink with the non-NULL
 * stream validated by ::ra8_io_log_attach), so it carries a GCOVR_EXCL_LINE
 * marker at its source rather than a fabricated test.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_io_log.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_ram.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/**
 * @enum t_io_log_const_t
 * @brief Fixture sizes for the log-capture RAM sink.
 */
typedef enum : uint32_t {
  k_t_log_cap = 128, /**< RAM sink capacity for the captured log line. */
} t_io_log_const_t;

/**
 * @brief Discard validation-path log bytes before a stream is attached.
 * @details Supplies a host-safe byte sink while the attach rejection vectors
 *          emit expected diagnostics, avoiding target-only ITM register reads.
 * @param[in] context Unused logger context.
 * @param[in] byte Unused emitted byte.
 * @return Nothing.
 * @pre The caller permits the emitted diagnostic byte to be discarded.
 * @pre The callback is invoked synchronously by the logger.
 * @post The byte is discarded without accessing target ITM registers.
 * @post No fixture storage or logger ownership state is modified.
 * @note File-local host-test sink; it owns no storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_discard_log_byte(void* context, uint8_t byte)
{
  (void)context;
  (void)byte;
}

/**
 * @brief Attach rejects NULL and an uninitialised (unbound) stream handle.
 *
 * @details
 * Covers both guards in ::ra8_io_log_attach: the RA8_CHECK_NULL_PTR pointer guard
 * and the `s->iface == nullptr` not-initialised guard reached with a
 * zero-initialised handle that was never bound to a sink.
 *
 * @par MC/DC:
 * Two independent single-condition guards, tested one vector each:
 * - `s == nullptr`      -> k_ra8_err_null_ptr        (pointer guard true).
 * - `s->iface == nullptr` -> k_ra8_err_not_initialized (iface guard true).
 * The valid-bound case in ::internal_test_attach_emit_detach supplies the false vector
 * for both guards (returns k_ra8_ok), completing independent influence. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_attach_rejects_invalid(void)
{
  TEST_BEGIN("attach rejects invalid");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_log_attach(nullptr));

  ra8_io_stream_t unbound = {}; /* iface == nullptr: never bound to a sink. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_io_log_attach(&unbound));
  TEST_END("attach rejects invalid");
}

/**
 * @brief Attach a bound stream, emit a log line, then detach.
 *
 * @details
 * Drives the false leg of both ::ra8_io_log_attach guards (a valid bound RAM
 * stream returns k_ra8_ok), routes a real ra8_log line through the installed byte
 * sink so the per-byte stream write runs, and restores the default backend via
 * ::ra8_io_log_detach. Confirms the tag and message landed in the capture buffer.
 *
 * @par MC/DC:
 * (no compound decisions under test -- a valid bound handle is accepted, the
 * captured buffer is inspected, and detach clears the sink) @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_attach_emit_detach(void)
{
  TEST_BEGIN("attach emit detach");
  static char               s_buf[(size_t)k_t_log_cap] = {};
  ra8_io_stream_ram_state_t st                         = {};
  ra8_io_stream_t           s                          = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_init(&s, &st, (uint8_t*)s_buf, k_t_log_cap - 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_log_attach(&s));
  ra8_log_init();
  ra8_log_error("IOL", "line");
  ra8_io_log_detach();

  uint32_t used = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_used(&st, &used));
  TEST_ASSERT(used > 0U);
  s_buf[used] = '\0';
  TEST_ASSERT(strstr(s_buf, "IOL") != nullptr);
  TEST_ASSERT(strstr(s_buf, "line") != nullptr);
  TEST_END("attach emit detach");
}

int main(void)
{
  ra8_log_set_byte_sink(internal_discard_log_byte, nullptr);
  internal_test_attach_rejects_invalid();
  internal_test_attach_emit_detach();
  return 0;
}
