/**
 * @file test_ra8_boot_region.c
 * @brief Unit tests for libs/ra8_core/src/ra8_boot_region.c
 *
 * @details
 * The unit under test supplies the zero-fill the reset handler cannot do for
 * `.sdram_data`, so the tests pin three things the caller depends on: the span
 * is half-open (the byte AT `end` survives), an empty span is a success that
 * writes nothing, and the section-level entry point really clears the window
 * it is handed rather than merely returning `k_ra8_ok`. The window on the host
 * is the file-static stand-in ::ra8_boot_test_sdram_window hands back, since
 * there is no linker script off target.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_boot_region.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief Fixture sizes and sentinel bytes. */
typedef enum : uint8_t {
  k_brt_buf_bytes   = 16U,   /**< Fixture buffer size in bytes.            */
  k_brt_dirty       = 0xA5U, /**< Non-zero fill so a missed byte is loud.   */
  k_brt_guard       = 0x5AU, /**< Distinct sentinel for out-of-span bytes.  */
  k_brt_span_first  = 4U,    /**< First index of the interior span cleared. */
  k_brt_span_last   = 12U,   /**< One past the last index of that span.     */
} brt_const_t;

/**
 * @brief Fill a buffer with the dirty pattern.
 *
 * @param[out] buf   Buffer to fill. Must not be nullptr.
 * @param[in]  bytes Number of bytes to fill.
 *
 * @since 0.1.0
 */
static void brt_dirty_fill(uint8_t* buf, size_t bytes)
{
  for (size_t i = 0U; i < bytes; i++) {
    buf[i] = (uint8_t)k_brt_dirty;
  }
}

/**
 * @brief Verify an interior span is cleared and neither neighbour moves.
 *
 * @details
 * The span `[4, 12)` sits inside a 16-byte buffer whose every byte starts
 * non-zero, so a fill that overruns either end fails on the guard bytes and a
 * fill that stops short fails on the cleared range. This is what makes the
 * contract half-open rather than inclusive.
 *
 * @pre None.
 * @post Bytes 4..11 read zero; bytes 0..3 and 12..15 keep the dirty pattern.
 *
 * @par MC/DC:
 * Decision: `last < first` in ra8_boot_zero_region()
 * - V1: end after start -> false, the fill runs (this test).
 * - V2: end before start -> true, rejected (see the rejection test).
 *
 * @since 0.1.0
 */
static void test_zero_region_clears_exact_span(void)
{
  TEST_BEGIN("zero_region clears the half-open span only");
  uint8_t buf[k_brt_buf_bytes];
  brt_dirty_fill(buf, (size_t)k_brt_buf_bytes);

  const ra8_err_t err = ra8_boot_zero_region(&buf[k_brt_span_first], &buf[k_brt_span_last]);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  for (uint8_t i = 0U; i < (uint8_t)k_brt_span_first; i++) {
    TEST_ASSERT_EQ((uint8_t)k_brt_dirty, buf[i]);
  }
  for (uint8_t i = (uint8_t)k_brt_span_first; i < (uint8_t)k_brt_span_last; i++) {
    TEST_ASSERT_EQ(0U, buf[i]);
  }
  for (uint8_t i = (uint8_t)k_brt_span_last; i < (uint8_t)k_brt_buf_bytes; i++) {
    TEST_ASSERT_EQ((uint8_t)k_brt_dirty, buf[i]);
  }
  TEST_END("zero_region clears the half-open span only");
}

/**
 * @brief Verify an empty span succeeds and writes nothing.
 *
 * @details
 * This is the shape every image that places nothing in SDRAM links: the two
 * section symbols resolve to the same address. It must not be an error, and it
 * must not touch the byte at that address.
 *
 * @pre None.
 * @post The buffer is unchanged and the call returned `k_ra8_ok`.
 *
 * @since 0.1.0
 */
static void test_zero_region_empty_span_is_ok(void)
{
  TEST_BEGIN("zero_region accepts an empty span and writes nothing");
  uint8_t buf[k_brt_buf_bytes];
  brt_dirty_fill(buf, (size_t)k_brt_buf_bytes);

  const ra8_err_t err = ra8_boot_zero_region(&buf[k_brt_span_first], &buf[k_brt_span_first]);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  for (uint8_t i = 0U; i < (uint8_t)k_brt_buf_bytes; i++) {
    TEST_ASSERT_EQ((uint8_t)k_brt_dirty, buf[i]);
  }
  TEST_END("zero_region accepts an empty span and writes nothing");
}

/**
 * @brief Verify the argument rejections, and that they write nothing.
 *
 * @details
 * A reversed span is the interesting one: a caller that swaps the two linker
 * symbols would otherwise walk backwards over live memory, so the check has to
 * reject rather than clamp. The nullptr arms pin which code each guard
 * returns, independently of the other.
 *
 * @pre None.
 * @post The buffer keeps its guard pattern.
 *
 * @since 0.1.0
 */
static void test_zero_region_rejections(void)
{
  TEST_BEGIN("zero_region rejects nullptr and a reversed span");
  uint8_t buf[k_brt_buf_bytes];
  for (uint8_t i = 0U; i < (uint8_t)k_brt_buf_bytes; i++) {
    buf[i] = (uint8_t)k_brt_guard;
  }

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_boot_zero_region(nullptr, &buf[k_brt_span_last]));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_boot_zero_region(&buf[0], nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_boot_zero_region(&buf[k_brt_span_last], &buf[k_brt_span_first]));

  for (uint8_t i = 0U; i < (uint8_t)k_brt_buf_bytes; i++) {
    TEST_ASSERT_EQ((uint8_t)k_brt_guard, buf[i]);
  }
  TEST_END("zero_region rejects nullptr and a reversed span");
}

/**
 * @brief Verify the section entry point clears the whole window it owns.
 *
 * @details
 * The window is dirtied through the test hook first, so a stub that returned
 * `k_ra8_ok` without writing fails here. On target the same call takes its
 * bounds from `g_ra8_ls_ssdram` / `g_ra8_ls_esdram`; the host stand-in is what
 * makes the body executable off target at all.
 *
 * @pre The stand-in window is non-empty.
 * @post Every byte of the window reads zero.
 *
 * @since 0.1.0
 */
static void test_zero_sdram_bss_clears_window(void)
{
  TEST_BEGIN("zero_sdram_bss clears the whole .sdram_data window");
  size_t         bytes  = 0U;
  uint8_t* const window = ra8_boot_test_sdram_window(&bytes);
  TEST_ASSERT(window != nullptr);
  TEST_ASSERT(bytes > 0U);

  brt_dirty_fill(window, bytes);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_boot_zero_sdram_bss());
  for (size_t i = 0U; i < bytes; i++) {
    TEST_ASSERT_EQ(0U, window[i]);
  }
  TEST_END("zero_sdram_bss clears the whole .sdram_data window");
}

int main(void)
{
  test_zero_region_clears_exact_span();
  test_zero_region_empty_span_is_ok();
  test_zero_region_rejections();
  test_zero_sdram_bss_clears_window();
  return 0;
}
