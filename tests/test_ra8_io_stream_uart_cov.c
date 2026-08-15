/**
 * @file test_ra8_io_stream_uart_cov.c
 * @brief Coverage-boost tests for ra8_io_stream_uart.c.
 *
 * @details
 * Targets the four source lines left uncovered after test_ra8_io_stream.c:
 *
 *   64  -- uart_write success path: `if (out_written != nullptr)` decision.
 *   65  -- uart_write success path: `*out_written = len` assignment (true arm).
 *   66  -- uart_write success path: closing brace of the out_written block.
 *   67  -- uart_write success path: `return k_ra8_ok` statement.
 *
 * The four lines are reached only when `ra8_sci_write_polling` returns
 * `k_ra8_ok`.  Using `len = 0` achieves this deterministically in
 * RA8_OFF_TARGET: the byte-loop is skipped and `internal_wait_tx_end`
 * is not reached, so no CSR.TDRE pre-seeding is required.
 *
 * A second suite pre-seeds CSR.TDRE and writes one real byte to confirm
 * that the same success path executes when the SCI transmit-data-register-
 * empty poll resolves immediately, as in test_ra8_uart.c.
 *
 * @par MC/DC:
 * Decision: `if (out_written != nullptr)` (1 condition, 2 distinct outcomes)
 * - Vector A: out_written = non-null  -> true  arm; lines 65-66 executed.
 * - Vector B: out_written = nullptr   -> false arm; lines 65-66 skipped.
 * Two vectors suffice for N+1 = 2 (N=1 condition): minimal MC/DC.
 *
 * This file is auto-discovered by the tests/CMakeLists.txt GLOB so no
 * manual list entry is required.  gcovr merges its .gcda counters with
 * sibling tests so newly executed lines accumulate toward the module
 * coverage total.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_uart.h"
#include "ra8_sci_regs.h"
#include "unity_minimal.h"

/**
 * @enum io_stream_uart_cov_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint32_t {
  k_io_poison_written =
    0xFFFFFFFFU, /**< Poison in the bytes-written out-param; a call skipping it is detectable. */
} io_stream_uart_cov_fixture_t;

/* =========================================================================
 * Fixture constants
 * =========================================================================
 */

/**
 * @enum uart_cov_const_t
 * @brief Symbolic constants for the UART stream coverage fixture.
 *
 * @details
 * Collects every numeric value used in this file so the magic-number gate
 * stays silent for first-party readers (tests are exempt from the gate but
 * named values aid comprehension and maintenance).
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_uart_cov_channel     = 0U,    /**< SCI channel 0 -- always in range 0..9. */
  k_uart_cov_channel_oor = 20U,   /**< Channel 20 is out of range (max is 9). */
  k_uart_cov_test_byte   = 0x41U, /**< ASCII 'A' -- single payload byte.      */
} uart_cov_const_t;

/* =========================================================================
 * Helpers
 * =========================================================================
 */

/**
 * @brief Reset all MMIO backing memory and pre-seed CSR.TDRE for channel 0.
 *
 * @details
 * Mirrors the technique used in test_ra8_uart.c: the fake peripheral
 * window is zeroed first, then the TDRE bit in CSR is set so that the
 * ra8_sci_putc_polling spin resolves on the first iteration without
 * busy-waiting.
 *
 * @pre RA8_OFF_TARGET is defined and ra8_fake_mmap_install has run.
 * @post CSR[k_ra8_sci_csr_bit_tdre] is 1 for SCI channel 0.
 * @post All other MMIO registers read zero.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. */
RA8_INTERNAL static void internal_fixture_seed_tdre(void)
{
  ra8_fake_mmap_reset();
  volatile r_sci_regs_t* reg = ra8_sci((uint8_t)k_uart_cov_channel);
  reg->CSR                   = (uint32_t)(1U << (uint8_t)k_ra8_sci_csr_bit_tdre);
}

/* =========================================================================
 * Test: write with out_written supplied (true arm of line 64)
 * =========================================================================
 */

/**
 * @brief Write zero bytes through a UART stream with out_written supplied.
 *
 * @details
 * Exercises the success path of uart_write (lines 64-67) via the
 * ra8_io_stream_write dispatcher.  len = 0 causes ra8_sci_write_polling to
 * return k_ra8_ok immediately (the byte loop and TEND wait are both skipped),
 * so no CSR.TDRE pre-seeding is required.  The non-null out_written pointer
 * ensures the true arm of `if (out_written != nullptr)` is taken (lines
 * 64-66).
 *
 * @par MC/DC:
 * Decision: `if (out_written != nullptr)` -- Vector A (true arm).
 * Pairs with internal_test_write_success_null_out_written (Vector B, false arm)
 * for full MC/DC coverage of the single-condition decision.
 *
 * @pre ra8_fake_mmap is installed (constructor, runs before main).
 * @pre SCI channel 0 is a valid channel (0..9).
 * @post out_written receives 0 (== len).
 * @post Stream handle is in a consistent bound state after the call.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_success_with_out_written(void)
{
  TEST_BEGIN("uart write success: out_written supplied");
  ra8_fake_mmap_reset();

  ra8_io_stream_t            s     = {};
  ra8_io_stream_uart_state_t state = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_uart_init(&s, &state, (uint8_t)k_uart_cov_channel));

  /* A non-null buf is required even for len=0 (uart_write checks it). */
  const uint8_t buf[1]  = {(uint8_t)k_uart_cov_test_byte};
  uint32_t      written = k_io_poison_written;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_write(&s, buf, 0U, &written));
  /* On success *out_written is set to len (== 0). */
  TEST_ASSERT_EQ(0U, written);

  TEST_END("uart write success: out_written supplied");
}

/* =========================================================================
 * Test: write with out_written == nullptr (false arm of line 64)
 * =========================================================================
 */

/**
 * @brief Write zero bytes through a UART stream with out_written = nullptr.
 *
 * @details
 * Exercises the same success path (lines 64, 67) but skips the
 * out_written block (lines 65-66) by passing nullptr.  Together with
 * internal_test_write_success_with_out_written this covers both outcomes of the
 * `if (out_written != nullptr)` decision.
 *
 * @par MC/DC:
 * Decision: `if (out_written != nullptr)` -- Vector B (false arm).
 * Pairs with internal_test_write_success_with_out_written (Vector A, true arm).
 *
 * @pre ra8_fake_mmap is installed (constructor, runs before main).
 * @pre SCI channel 0 is a valid channel (0..9).
 * @post No side effects beyond the internal write (len = 0 transmits nothing).
 * @post Stream handle remains in a consistent bound state.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_success_null_out_written(void)
{
  TEST_BEGIN("uart write success: out_written nullptr");
  ra8_fake_mmap_reset();

  ra8_io_stream_t            s     = {};
  ra8_io_stream_uart_state_t state = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_uart_init(&s, &state, (uint8_t)k_uart_cov_channel));

  const uint8_t buf[1] = {(uint8_t)k_uart_cov_test_byte};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_write(&s, buf, 0U, nullptr));

  TEST_END("uart write success: out_written nullptr");
}

/* =========================================================================
 * Test: write one real byte (TDRE pre-seeded) with out_written supplied
 * =========================================================================
 */

/**
 * @brief Write one byte through a UART stream with TDRE pre-seeded.
 *
 * @details
 * Confirms that the success path (lines 64-67) is also reached when the
 * byte loop executes: CSR.TDRE is pre-seeded so ra8_sci_putc_polling
 * resolves on the first poll, and ra8_sci_write_polling returns k_ra8_ok.
 * out_written receives 1 (== len).
 *
 * @par MC/DC:
 * (no compound decisions exercised beyond those in the sibling tests;
 * this case is a second true-arm vector for the out_written branch,
 * confirming the assignment `*out_written = len` with len = 1)
 *
 * @pre ra8_fake_mmap is installed (constructor, runs before main).
 * @pre CSR.TDRE is pre-set so the TDRE spin completes immediately.
 * @post out_written receives 1 (== len).
 * @post TDR holds the transmitted byte.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_one_byte_tdre_seeded(void)
{
  TEST_BEGIN("uart write one byte: TDRE pre-seeded, out_written supplied");
  internal_fixture_seed_tdre();

  ra8_io_stream_t            s     = {};
  ra8_io_stream_uart_state_t state = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_uart_init(&s, &state, (uint8_t)k_uart_cov_channel));

  const uint8_t buf[1]  = {(uint8_t)k_uart_cov_test_byte};
  uint32_t      written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_write(&s, buf, 1U, &written));
  /* ra8_sci_write_polling consumed one byte; out_written == len == 1. */
  TEST_ASSERT_EQ(1U, written);

  TEST_END("uart write one byte: TDRE pre-seeded, out_written supplied");
}

/* =========================================================================
 * Test: flush through the UART stream
 * =========================================================================
 */

/**
 * @brief Flush a UART stream after a zero-byte write.
 *
 * @details
 * Drives uart_flush through the ra8_io_stream_flush dispatcher.  In
 * RA8_OFF_TARGET internal_wait_tx_end returns k_ra8_ok immediately, so
 * this test is deterministic with no pre-seeding required.
 *
 * @par MC/DC:
 * (no compound decisions in uart_flush itself; single-condition guard on
 * ctx that is always true here)
 *
 * @pre ra8_fake_mmap is installed.
 * @pre SCI channel 0 is valid.
 * @post Flush returns k_ra8_ok.
 * @post No MMIO registers are mutated by a zero-byte send + flush.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_flush_uart_stream(void)
{
  TEST_BEGIN("uart flush: returns ok in off-target mode");
  ra8_fake_mmap_reset();

  ra8_io_stream_t            s     = {};
  ra8_io_stream_uart_state_t state = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_uart_init(&s, &state, (uint8_t)k_uart_cov_channel));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_flush(&s));

  TEST_END("uart flush: returns ok in off-target mode");
}

/* =========================================================================
 * Test runner
 * =========================================================================
 */

/**
 * @brief Entry point for the uart coverage test executable.
 *
 * @details
 * Runs every test in declaration order and returns 0 on success.  Any
 * assertion failure exits with a non-zero status via the TEST_FAIL_FMT
 * macro in unity_minimal.h.
 *
 * @return int32_t 0 on success.
 *
 * @pre All MMIO windows are mapped (ra8_fake_mmap_install constructor has run).
 * @pre The test binary is compiled with RA8_OFF_TARGET defined.
 * @post Each test function has executed.
 * @post All assertions passed (or the process exited early on failure).
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
int32_t main(void)
{
  internal_test_write_success_with_out_written();
  internal_test_write_success_null_out_written();
  internal_test_write_one_byte_tdre_seeded();
  internal_test_flush_uart_stream();
  return 0;
}
