/**
 * @file test_ra8_slab.c
 * @brief Unit tests for the ra8_mem fixed-cell slab allocator (Layer 0, #147).
 *
 * @details
 * Exercises init geometry, O(1) alloc/exhaust/free/reuse, freelist integrity
 * (all cells distinct + in range, full drain and refill), the stats counters,
 * and every validation guard (NULL args, bad cell size/alignment, zero-cell
 * buffer, out-of-range / misaligned free).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_slab.h"
#include "unity_minimal.h"

/** @brief Distinct fills proving two slab cells do not alias. */
typedef enum : uint8_t {
  k_slab_fill_cell_a = 0xAAU, /**< Written through the first cell.  */
  k_slab_fill_cell_b = 0x55U, /**< Written through the second cell. */
} slab_fill_t;

/**
 * @enum t_slab_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_cell_bytes = 16U, /**< Cell size under test. */
  k_t_cells      = 8U,  /**< Cells in the pool.    */
} t_slab_const_t;

static uint8_t s_pool[(size_t)k_t_cells * (size_t)k_t_cell_bytes];

/**
 * @brief Verify slab initialization, unique allocation, exhaustion, and
 * statistics.
 * @details Drains every fixed cell, checks range and alignment, proves pairwise
 * uniqueness, and observes the empty free count.
 * @pre The file-scope pool is writable and sized to an integral cell count.
 * @pre The configured cell size satisfies the allocator's alignment contract.
 * @post Exactly the configured number of distinct cells are returned.
 * @post One further allocation reports no memory and free count is zero.
 * @note Pairwise comparison makes freelist aliasing visible independently of
 * payload writes.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- init builds N cells all free; alloc
 * hands out N distinct in-range cells then reports no_mem; stats track the
 * drain)
 */
RA8_INTERNAL static void internal_test_init_alloc_exhaust(void)
{
  TEST_BEGIN("slab init / alloc / exhaust");
  ra8_slab_t slab = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_init(&slab, s_pool, sizeof(s_pool), k_t_cell_bytes));
  uint32_t freec = 0;
  uint32_t total = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_stats(&slab, &freec, &total));
  TEST_ASSERT_EQ(k_t_cells, freec);
  TEST_ASSERT_EQ(k_t_cells, total);

  void* cells[(size_t)k_t_cells] = {};
  for (uint32_t i = 0; i < (uint32_t)k_t_cells; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_alloc(&slab, &cells[i]));
    TEST_ASSERT(cells[i] != nullptr);
    /* in range and on a cell boundary */
    const uintptr_t off = (uintptr_t)cells[i] - (uintptr_t)s_pool;
    TEST_ASSERT(off < sizeof(s_pool));
    TEST_ASSERT_EQ(0U, (off % (uintptr_t)k_t_cell_bytes));
  }
  /* all cells distinct (no overlap) */
  for (uint32_t i = 0; i < (uint32_t)k_t_cells; ++i) {
    for (uint32_t j = i + 1U; j < (uint32_t)k_t_cells; ++j) {
      TEST_ASSERT(cells[i] != cells[j]);
    }
  }
  /* exhausted */
  void* extra = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_slab_alloc(&slab, &extra));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_stats(&slab, &freec, nullptr));
  TEST_ASSERT_EQ(0U, freec);
  TEST_END("slab init / alloc / exhaust");
}

/**
 * @brief Verify freed slab cells are reusable and full capacity is recoverable.
 * @details Writes distinct cells, checks LIFO reuse, drains the slab, then
 * frees every live allocation.
 * @pre The slab pool can be initialized with the fixture geometry.
 * @pre Every pointer returned by allocation is retained until its matching
 * free.
 * @post The first replacement allocation reuses the most recently freed cell.
 * @post Final free count returns to the configured total.
 * @note Distinct fill bytes additionally prove the first two cells do not
 * alias.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a freed cell is handed back out, and a
 * full drain then full free restores the original free count)
 */
RA8_INTERNAL static void internal_test_free_reuse(void)
{
  TEST_BEGIN("slab free / reuse / refill");
  ra8_slab_t slab = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_init(&slab, s_pool, sizeof(s_pool), k_t_cell_bytes));

  void* a = nullptr;
  void* b = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_alloc(&slab, &a));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_alloc(&slab, &b));
  /* write through the cells to confirm they are usable, distinct storage */
  (void)memset(a, k_slab_fill_cell_a, (size_t)k_t_cell_bytes);
  (void)memset(b, k_slab_fill_cell_b, (size_t)k_t_cell_bytes);
  TEST_ASSERT_EQ(0xAA, ((uint8_t*)a)[0]);
  TEST_ASSERT_EQ(0x55, ((uint8_t*)b)[0]);

  /* free one, the next alloc reuses it (LIFO freelist) */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_free(&slab, a));
  void* c = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_alloc(&slab, &c));
  TEST_ASSERT(c == a);

  /* drain fully, then free everything: free count returns to the total */
  void*    rest[(size_t)k_t_cells] = {};
  uint32_t n                       = 0;
  while (ra8_slab_alloc(&slab, &rest[n]) == k_ra8_ok) {
    ++n;
  }
  uint32_t freec = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_stats(&slab, &freec, nullptr));
  TEST_ASSERT_EQ(0U, freec);
  for (uint32_t i = 0; i < n; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_free(&slab, rest[i]));
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_free(&slab, b));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_free(&slab, c));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_stats(&slab, &freec, nullptr));
  TEST_ASSERT_EQ(k_t_cells, freec);
  TEST_END("slab free / reuse / refill");
}

/**
 * @brief Verify slab initialization, operation, and pointer-boundary guards.
 * @details Covers null arguments, malformed geometry, and below, beyond, or
 * mid-cell free pointers.
 * @pre The file-scope pool and a separate outside buffer are addressable.
 * @pre The valid fixture geometry can initialize the slab before operation
 * guards run.
 * @post Each invalid configuration reports null, invalid-size, or
 * invalid-argument as appropriate.
 * @post No rejected free changes the allocator's ownership state.
 * @note The below-base check is conditional because stack and static address
 * ordering is platform-dependent.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition check)
 */
RA8_INTERNAL static void internal_test_validation(void)
{
  TEST_BEGIN("slab validation");
  ra8_slab_t slab = {};
  /* init guards */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_slab_init(nullptr, s_pool, sizeof(s_pool), k_t_cell_bytes));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_slab_init(&slab, nullptr, sizeof(s_pool), k_t_cell_bytes));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_slab_init(&slab, s_pool, sizeof(s_pool), 2U)); /* < 4 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_slab_init(&slab, s_pool, sizeof(s_pool), 6U));              /* not 4-aligned */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_slab_init(&slab, s_pool, 8U, 16U)); /* zero cells    */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_init(&slab, s_pool, sizeof(s_pool), k_t_cell_bytes));
  /* alloc/free/stats null guards */
  void* cell = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_slab_alloc(nullptr, &cell));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_slab_alloc(&slab, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_slab_free(nullptr, s_pool));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_slab_free(&slab, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_slab_stats(nullptr, nullptr, nullptr));

  /* free of a pointer below the pool, past the pool, and mid-cell */
  uint8_t outside[16] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_slab_free(&slab, &s_pool[sizeof(s_pool)]));           /* past end */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_slab_free(&slab, &s_pool[1])); /* mid-cell */
  if ((uintptr_t)outside < (uintptr_t)s_pool) {
    TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_slab_free(&slab, outside)); /* below base */
  }
  TEST_END("slab validation");
}

/**
 * @brief Consume one host-test log byte without touching target ITM MMIO.
 * @details Implements the injected logger sink as an intentional no-op for expected-error vectors.
 * @param[in] context Unused sink context.
 * @param[in] byte Unused diagnostic byte emitted by the production path.
 * @pre The test process owns the logger sink for the suite lifetime.
 * @pre No vector depends on observing diagnostic text.
 * @post No memory, descriptor, or hardware state is modified.
 * @post Control returns to the production logger immediately.
 * @note Installing this sink keeps sanitizer runs away from the target-only ITM address window.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_host_log_sink(void* context, uint8_t byte)
{
  (void)context;
  (void)byte;
}

int32_t main(void)
{
  ra8_log_set_byte_sink(internal_host_log_sink, nullptr);
  internal_test_init_alloc_exhaust();
  internal_test_free_reuse();
  internal_test_validation();
  ra8_log_set_byte_sink(nullptr, nullptr);
  return 0;
}
