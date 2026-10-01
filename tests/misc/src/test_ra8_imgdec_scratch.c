/**
 * @file test_ra8_imgdec_scratch.c
 * @brief Host tests for the shared decoder bump scratch (RA8FW-308).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_imgdec_scratch.h"
#include "unity_minimal.h"

/** @brief Fixture sizes (no magic numbers). */
enum : uint32_t {
  k_store_bytes = 256, /**< Backing store every case draws from. */
  k_small_bytes = 8,   /**< Request smaller than the alignment.  */
  k_fill_byte   = 0xA5 /**< Non-zero pre-fill, so calloc proves. */
};

static uint8_t g_store[k_store_bytes];

/**
 * @brief Fresh scratch over the shared backing store, pre-filled non-zero.
 *
 * @param[out] scratch Scratch to initialise.
 *
 * @return None.
 */
RA8_INTERNAL static void internal_fresh(ra8_imgdec_scratch_t* scratch) {
  (void)memset(g_store, k_fill_byte, sizeof(g_store));
  *scratch = (ra8_imgdec_scratch_t){0};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_scratch_init(scratch, g_store, sizeof(g_store)));
}

RA8_INTERNAL static void internal_test_init_guards(void) {
  TEST_BEGIN("init refuses a missing record, store or capacity");

  ra8_imgdec_scratch_t scratch = {0};

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_imgdec_scratch_init(nullptr, g_store, sizeof(g_store)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_imgdec_scratch_init(&scratch, nullptr, sizeof(g_store)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_scratch_init(&scratch, g_store, 0U));

  /* A refused init leaves the record exactly as it was. */
  TEST_ASSERT_NULL(scratch.base);
  TEST_ASSERT(scratch.cap == 0U);

  TEST_END("init refuses a missing record, store or capacity");
}

RA8_INTERNAL static void internal_test_alloc_aligns(void) {
  TEST_BEGIN("every block is alignment-rounded and inside the store");

  ra8_imgdec_scratch_t scratch;
  internal_fresh(&scratch);

  uint8_t* const first = (uint8_t*)ra8_imgdec_scratch_alloc(&scratch, k_small_bytes);
  TEST_ASSERT_NOT_NULL(first);
  TEST_ASSERT(first == g_store);

  uint8_t* const second = (uint8_t*)ra8_imgdec_scratch_alloc(&scratch, k_small_bytes);
  TEST_ASSERT_NOT_NULL(second);

  /* Eight bytes asked, sixteen consumed: the next block starts aligned. */
  const size_t gap = (size_t)(second - first);
  TEST_ASSERT(gap == (size_t)k_ra8_imgdec_scratch_align);
  TEST_ASSERT(scratch.live == 2U);
  TEST_ASSERT(scratch.offset == (2U * (size_t)k_ra8_imgdec_scratch_align));

  TEST_END("every block is alignment-rounded and inside the store");
}

RA8_INTERNAL static void internal_test_alloc_refuses(void) {
  TEST_BEGIN("a request past the capacity is refused, not clamped");

  ra8_imgdec_scratch_t scratch;
  internal_fresh(&scratch);

  TEST_ASSERT_NULL(ra8_imgdec_scratch_alloc(&scratch, 0U));
  TEST_ASSERT_NULL(ra8_imgdec_scratch_alloc(nullptr, k_small_bytes));
  TEST_ASSERT_NULL(ra8_imgdec_scratch_alloc(&scratch, sizeof(g_store) + 1U));
  TEST_ASSERT_NULL(ra8_imgdec_scratch_alloc(&scratch, SIZE_MAX));

  /* Nothing was consumed by any refusal. */
  TEST_ASSERT(scratch.offset == 0U);
  TEST_ASSERT(scratch.live == 0U);

  TEST_ASSERT_NOT_NULL(ra8_imgdec_scratch_alloc(&scratch, sizeof(g_store)));
  TEST_ASSERT_NULL(ra8_imgdec_scratch_alloc(&scratch, k_small_bytes));

  TEST_END("a request past the capacity is refused, not clamped");
}

RA8_INTERNAL static void internal_test_free_rewinds(void) {
  TEST_BEGIN("the cursor rewinds only when the last block is released");

  ra8_imgdec_scratch_t scratch;
  internal_fresh(&scratch);

  void* const a = ra8_imgdec_scratch_alloc(&scratch, k_small_bytes);
  void* const b = ra8_imgdec_scratch_alloc(&scratch, k_small_bytes);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);

  /* Freed out of order, which is what stb_image actually does. */
  ra8_imgdec_scratch_free(&scratch, a);
  TEST_ASSERT(scratch.live == 1U);
  TEST_ASSERT(scratch.offset != 0U);

  ra8_imgdec_scratch_free(&scratch, b);
  TEST_ASSERT(scratch.live == 0U);
  TEST_ASSERT(scratch.offset == 0U);

  /* Over-releasing and releasing nothing are both ignored. */
  ra8_imgdec_scratch_free(&scratch, b);
  ra8_imgdec_scratch_free(&scratch, nullptr);
  ra8_imgdec_scratch_free(nullptr, b);
  TEST_ASSERT(scratch.live == 0U);

  TEST_END("the cursor rewinds only when the last block is released");
}

RA8_INTERNAL static void internal_test_calloc_zeroes(void) {
  TEST_BEGIN("calloc zeroes its block and refuses an overflowing product");

  ra8_imgdec_scratch_t scratch;
  internal_fresh(&scratch);

  uint8_t* const block = (uint8_t*)ra8_imgdec_scratch_calloc(&scratch, 4U, 4U);
  TEST_ASSERT_NOT_NULL(block);

  bool all_zero = true;
  for (size_t i = 0U; i < 16U; ++i) {
    if (block[i] != 0U) {
      all_zero = false;
    }
  }
  TEST_ASSERT(all_zero);

  TEST_ASSERT_NULL(ra8_imgdec_scratch_calloc(&scratch, 0U, 4U));
  TEST_ASSERT_NULL(ra8_imgdec_scratch_calloc(&scratch, 4U, 0U));
  TEST_ASSERT_NULL(ra8_imgdec_scratch_calloc(&scratch, SIZE_MAX, 2U));
  TEST_ASSERT(scratch.live == 1U);

  TEST_END("calloc zeroes its block and refuses an overflowing product");
}

RA8_INTERNAL static void internal_test_realloc_carries(void) {
  TEST_BEGIN("realloc carries the old contents and drops the old block");

  ra8_imgdec_scratch_t scratch;
  internal_fresh(&scratch);

  uint8_t* const first = (uint8_t*)ra8_imgdec_scratch_alloc(&scratch, k_small_bytes);
  TEST_ASSERT_NOT_NULL(first);
  for (size_t i = 0U; i < (size_t)k_small_bytes; ++i) {
    first[i] = (uint8_t)(i + 1U);
  }

  uint8_t* const grown = (uint8_t*)ra8_imgdec_scratch_realloc(&scratch, first, k_small_bytes, 64U);
  TEST_ASSERT_NOT_NULL(grown);
  TEST_ASSERT(grown != first);
  TEST_ASSERT(scratch.live == 1U);

  bool carried = true;
  for (size_t i = 0U; i < (size_t)k_small_bytes; ++i) {
    if (grown[i] != (uint8_t)(i + 1U)) {
      carried = false;
    }
  }
  TEST_ASSERT(carried);

  /* A null pointer makes it a plain allocation; a zero size is a refusal. */
  TEST_ASSERT_NOT_NULL(ra8_imgdec_scratch_realloc(&scratch, nullptr, 0U, k_small_bytes));
  TEST_ASSERT_NULL(ra8_imgdec_scratch_realloc(&scratch, grown, 64U, 0U));

  TEST_END("realloc carries the old contents and drops the old block");
}

RA8_INTERNAL static void internal_test_realloc_failure_keeps_old(void) {
  TEST_BEGIN("a refused grow leaves the old block and the scratch intact");

  ra8_imgdec_scratch_t scratch;
  internal_fresh(&scratch);

  uint8_t* const held = (uint8_t*)ra8_imgdec_scratch_alloc(&scratch, k_small_bytes);
  TEST_ASSERT_NOT_NULL(held);
  held[0] = k_fill_byte;

  const size_t before = scratch.offset;

  TEST_ASSERT_NULL(ra8_imgdec_scratch_realloc(&scratch, held, k_small_bytes, sizeof(g_store)));
  TEST_ASSERT(scratch.offset == before);
  TEST_ASSERT(scratch.live == 1U);
  TEST_ASSERT(held[0] == (uint8_t)k_fill_byte);

  TEST_END("a refused grow leaves the old block and the scratch intact");
}

RA8_INTERNAL static void internal_test_high_water_survives_reset(void) {
  TEST_BEGIN("the peak survives a reset, the occupancy does not");

  ra8_imgdec_scratch_t scratch;
  internal_fresh(&scratch);

  TEST_ASSERT(ra8_imgdec_scratch_high_water(&scratch) == 0U);
  TEST_ASSERT(ra8_imgdec_scratch_high_water(nullptr) == 0U);

  TEST_ASSERT_NOT_NULL(ra8_imgdec_scratch_alloc(&scratch, 64U));
  TEST_ASSERT_NOT_NULL(ra8_imgdec_scratch_alloc(&scratch, 64U));
  const size_t peak = ra8_imgdec_scratch_high_water(&scratch);
  TEST_ASSERT(peak == 128U);

  ra8_imgdec_scratch_reset(&scratch);
  TEST_ASSERT(scratch.offset == 0U);
  TEST_ASSERT(scratch.live == 0U);
  TEST_ASSERT(ra8_imgdec_scratch_high_water(&scratch) == peak);

  /* A shallower run afterwards does not lower the recorded peak. */
  TEST_ASSERT_NOT_NULL(ra8_imgdec_scratch_alloc(&scratch, k_small_bytes));
  TEST_ASSERT(ra8_imgdec_scratch_high_water(&scratch) == peak);

  ra8_imgdec_scratch_reset(nullptr);

  TEST_END("the peak survives a reset, the occupancy does not");
}

RA8_INTERNAL static void internal_test_drain_reuses_the_store(void) {
  TEST_BEGIN("a drained scratch hands the same bytes out again");

  ra8_imgdec_scratch_t scratch;
  internal_fresh(&scratch);

  void* const first = ra8_imgdec_scratch_alloc(&scratch, sizeof(g_store));
  TEST_ASSERT_NOT_NULL(first);
  TEST_ASSERT_NULL(ra8_imgdec_scratch_alloc(&scratch, k_small_bytes));

  ra8_imgdec_scratch_free(&scratch, first);

  void* const second = ra8_imgdec_scratch_alloc(&scratch, sizeof(g_store));
  TEST_ASSERT_NOT_NULL(second);
  TEST_ASSERT(second == first);

  TEST_END("a drained scratch hands the same bytes out again");
}

int main(void) {
  internal_test_init_guards();
  internal_test_alloc_aligns();
  internal_test_alloc_refuses();
  internal_test_free_rewinds();
  internal_test_calloc_zeroes();
  internal_test_realloc_carries();
  internal_test_realloc_failure_keeps_old();
  internal_test_high_water_survives_reset();
  internal_test_drain_reuses_the_store();
  return 0;
}
