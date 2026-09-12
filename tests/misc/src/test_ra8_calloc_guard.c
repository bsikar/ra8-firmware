/**
 * @file test_ra8_calloc_guard.c
 * @brief Unit tests for bounded ThreadX calloc multiplication and pool guards.
 *
 * @details
 * Verifies that custom calloc hook implementations protect against integer
 * multiplication overflow prior to arithmetic evaluation, handle zero-length
 * requests gracefully, preserve zero initialization on successful allocations,
 * and fail closed on pool exhaustion.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unity_minimal.h"

/**
 * @enum t_calloc_limits_t
 * @brief Constants for mock pool geometry and test sizing.
 */
typedef enum : size_t {
  k_mock_pool_capacity  = 256U, /**< Bounded mock pool storage capacity in bytes. */
  k_elem_size_small     = 4U,   /**< 4-byte element size.                         */
  k_elem_size_medium    = 16U,  /**< 16-byte element size.                        */
  k_elem_count_nominal  = 8U,   /**< Nominal element count.                       */
  k_elem_count_oversize = 100U, /**< Count exceeding pool capacity.               */
} t_calloc_limits_t;

/**
 * @struct mock_pool_t
 * @brief State tracking for a mock deterministic byte pool.
 */
typedef struct {
  uint8_t storage[k_mock_pool_capacity]; /**< Backing memory.            */
  size_t  used;                          /**< Currently allocated bytes. */
  size_t  alloc_calls;                   /**< Total allocation attempts. */
} mock_pool_t;

static mock_pool_t s_mock_pool;

/**
 * @brief Reset the mock pool tracker.
 * @details Clears the allocation count, used bytes, and zeroes backing storage.
 * @pre s_mock_pool is in addressable memory.
 * @pre No concurrent allocations are active.
 * @post s_mock_pool.used is zero.
 * @post s_mock_pool.alloc_calls is zero.
 * @note Used between tests to ensure deterministic isolation.
 * @since 0.1.0
 */
static void internal_mock_pool_reset(void)
{
  (void)memset(&s_mock_pool, 0, sizeof(s_mock_pool));
}

/**
 * @brief Mock implementation of tx_byte_allocate.
 * @details Simulates nonblocking ThreadX byte pool allocation.
 * @param[in,out] pool Pointer to mock pool context.
 * @param[out] memory_ptr Pointer where allocated address is written.
 * @param[in] memory_size Bytes requested.
 * @return 0 on success, 1 on pool exhaustion.
 * @pre pool != nullptr.
 * @pre memory_ptr != nullptr.
 * @post On success, *memory_ptr points to allocated span.
 * @post On failure, *memory_ptr is set to nullptr.
 * @note ThreadX returns TX_SUCCESS (0) on success.
 * @since 0.1.0
 */
static int internal_mock_allocate(mock_pool_t* pool, void** memory_ptr, size_t memory_size)
{
  pool->alloc_calls++;
  if ((pool->used + memory_size) > k_mock_pool_capacity) {
    *memory_ptr = nullptr;
    return 1;
  }
  *memory_ptr = &pool->storage[pool->used];
  pool->used += memory_size;
  return 0;
}

/**
 * @brief Production-equivalent hardened calloc implementation under test.
 * @details Validates non-zero request dimensions and guards multiplication against
 * integer overflow before performing nonblocking pool allocation and zero-filling.
 * @param[in] n Element count.
 * @param[in] size Element size in bytes.
 * @return Pointer to zeroed storage, or nullptr on failure/overflow.
 * @retval nullptr n == 0, size == 0, multiplication overflows, or pool is exhausted.
 * @retval non-null Pointer to zero-initialized allocated memory.
 * @pre s_mock_pool has been initialized.
 * @pre Caller checks returned pointer against nullptr.
 * @post On success, exactly n * size bytes are zero-initialized.
 * @post On failure, mock pool used bytes remain unchanged.
 * @note Matches the hardened pattern in demo_calloc and internal_demo_calloc.
 * @since 0.1.0
 */
static void* internal_guarded_calloc(size_t n, size_t size)
{
  if ((n == 0U) || (size == 0U)) {
    return nullptr;
  }
  if (n > (SIZE_MAX / size)) {
    return nullptr;
  }
  const size_t total = n * size;
  void*        p     = nullptr;
  if (internal_mock_allocate(&s_mock_pool, &p, total) != 0) {
    return nullptr;
  }
  (void)memset(p, 0, total);
  return p;
}

/**
 * @brief Test zero-dimension calloc semantics.
 * @details Verifies that n=0, size=0, and both=0 return nullptr and do not allocate.
 * @pre Mock pool is reset.
 * @pre guarded_calloc is reachable.
 * @post Pool used bytes remain zero.
 * @post Zero allocation calls reach the pool backend.
 * @note Zero dimensions must fail closed without querying the underlying pool.
 * @since 0.1.0
 * @par MC/DC:
 * Decision 1: ((n == 0U) || (size == 0U))
 * Vectors: (T, X) -> return nullptr, (F, T) -> return nullptr, (F, F) -> continue
 */
static void internal_test_zero_dimensions(void)
{
  TEST_BEGIN("calloc zero dimensions");
  internal_mock_pool_reset();

  TEST_ASSERT_NULL(internal_guarded_calloc(0U, k_elem_size_small));
  TEST_ASSERT_EQ(0U, s_mock_pool.alloc_calls);

  TEST_ASSERT_NULL(internal_guarded_calloc(k_elem_count_nominal, 0U));
  TEST_ASSERT_EQ(0U, s_mock_pool.alloc_calls);

  TEST_ASSERT_NULL(internal_guarded_calloc(0U, 0U));
  TEST_END("calloc zero dimensions");
}

/**
 * @brief Test nominal non-overflowing allocations.
 * @details Verifies that ordinary sizes succeed, allocate correct bytes, and zero buffer.
 * @pre Mock pool is reset.
 * @pre Requested bytes are within mock pool capacity.
 * @post Allocated pointer is non-null.
 * @post Allocated buffer bytes are verified zero.
 * @note Tests standard successful allocation behavior.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions under test -- standard success paths)
 */
static void internal_test_nominal_allocation(void)
{
  TEST_BEGIN("calloc nominal allocation");
  internal_mock_pool_reset();

  /* Pre-fill storage with non-zero marker bytes to prove zeroing */
  (void)memset(s_mock_pool.storage, 0xA5, sizeof(s_mock_pool.storage));

  void* p = internal_guarded_calloc(k_elem_count_nominal, k_elem_size_medium);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQ(1U, s_mock_pool.alloc_calls);
  TEST_ASSERT_EQ((k_elem_count_nominal * k_elem_size_medium), s_mock_pool.used);

  /* Verify zero initialization */
  const uint8_t* bytes = (const uint8_t*)p;
  for (size_t i = 0U; i < (size_t)(k_elem_count_nominal * k_elem_size_medium); ++i) {
    TEST_ASSERT_EQ(0U, bytes[i]);
  }
  TEST_END("calloc nominal allocation");
}

/**
 * @brief Test multiplication overflow prevention.
 * @details Verifies obvious overflows, wraparound cases, and exact boundaries.
 * @pre Mock pool is reset.
 * @pre Multiplication factors exceed or equal overflow thresholds.
 * @post All overflow vectors return nullptr.
 * @post No allocation call reaches the pool backend.
 * @note Arithmetic guard must fire before multiplication occurs.
 * @since 0.1.0
 * @par MC/DC:
 * Decision 2: (n > (SIZE_MAX / size))
 * Vectors: T -> return nullptr, F -> continue to pool allocate
 */
static void internal_test_overflow_prevention(void)
{
  TEST_BEGIN("calloc overflow prevention");
  internal_mock_pool_reset();

  /* Obvious overflow: SIZE_MAX * 2 */
  TEST_ASSERT_NULL(internal_guarded_calloc(SIZE_MAX, 2U));
  TEST_ASSERT_EQ(0U, s_mock_pool.alloc_calls);

  /* Obvious overflow: 2 * SIZE_MAX */
  TEST_ASSERT_NULL(internal_guarded_calloc(2U, SIZE_MAX));
  TEST_ASSERT_EQ(0U, s_mock_pool.alloc_calls);

  /* Wraparound case: (SIZE_MAX / 2) + 1 multiplied by 2 wraps to 0 or small */
  const size_t wrap_count = (SIZE_MAX / 2U) + 1U;
  TEST_ASSERT_NULL(internal_guarded_calloc(wrap_count, 2U));
  TEST_ASSERT_EQ(0U, s_mock_pool.alloc_calls);

  /* Wraparound case: 4-byte element wraparound */
  const size_t wrap_count4 = (SIZE_MAX / 4U) + 2U;
  TEST_ASSERT_NULL(internal_guarded_calloc(wrap_count4, 4U));
  TEST_ASSERT_EQ(0U, s_mock_pool.alloc_calls);

  /* Boundary test: largest non-overflowing request (SIZE_MAX / size) * size */
  /* This calculation does NOT overflow, but exceeds pool capacity */
  const size_t max_valid_count = SIZE_MAX / k_elem_size_small;
  TEST_ASSERT_NULL(internal_guarded_calloc(max_valid_count, k_elem_size_small));
  /* It passes the overflow check, reaches the pool, and fails on pool capacity */
  TEST_ASSERT_EQ(1U, s_mock_pool.alloc_calls);

  /* Just beyond boundary: (SIZE_MAX / size) + 1 overflows */
  TEST_ASSERT_NULL(internal_guarded_calloc(max_valid_count + 1U, k_elem_size_small));
  /* Fails overflow check directly, no new pool call */
  TEST_ASSERT_EQ(1U, s_mock_pool.alloc_calls);
  TEST_END("calloc overflow prevention");
}

/**
 * @brief Test pool exhaustion failure propagation.
 * @details Verifies that requests exceeding remaining pool capacity fail closed.
 * @pre Mock pool is reset.
 * @pre Request size is non-overflowing but exceeds pool capacity.
 * @post guarded_calloc returns nullptr.
 * @post Pool used bytes remain unchanged after failure.
 * @note Confirms deterministic memory exhaustion handling.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions under test -- checks allocator error return)
 */
static void internal_test_pool_exhaustion(void)
{
  TEST_BEGIN("calloc pool exhaustion");
  internal_mock_pool_reset();
  void* p = internal_guarded_calloc(k_elem_count_oversize, k_elem_size_small);
  TEST_ASSERT_NULL(p);
  TEST_ASSERT_EQ(1U, s_mock_pool.alloc_calls);
  TEST_ASSERT_EQ(0U, s_mock_pool.used);
  TEST_END("calloc pool exhaustion");
}

int main(void)
{
  internal_test_zero_dimensions();
  internal_test_nominal_allocation();
  internal_test_overflow_prevention();
  internal_test_pool_exhaustion();
  return 0;
}
