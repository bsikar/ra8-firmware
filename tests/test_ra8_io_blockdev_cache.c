/**
 * @file test_ra8_io_blockdev_cache.c
 * @brief Unit tests for the ra8_io caching block device (issue #160).
 *
 * @details
 * Wraps a RAM block device with a 2-slot LRU cache and checks: read hit vs miss
 * accounting, LRU eviction, write-through (the backend sees the write and a
 * follow-up read hits the cache), erase invalidation, and init validation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_cache.h"
#include "ra8_io_blockdev_ram.h"
#include "unity_minimal.h"

/** @brief Poison byte a cache hit must overwrite. */
typedef enum : uint8_t {
  k_cache_fill_poison = 0x3CU, /**< Never a valid cached byte. */
} cache_fill_t;

/**
 * @enum io_blockdev_cache_fixture_t
 * @brief The payload generators and their seeds.
 */
typedef enum : uint8_t {
  k_bdc_seed_block1 = 0x11, /**< Fill seed for logical block 1.                                  */
  k_bdc_seed_block2 = 0x12, /**< For block 2, adjacent to it so an off-by-one line is caught.    */
  k_bdc_seed_block4 = 0x44, /**< For block 4, far enough away to land in a different cache line. */
} io_blockdev_cache_fixture_t;

/**
 * @enum t_cache_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_under_blocks = 8, /**< RAM backend size. */
  k_t_cache_slots  = 2, /**< Cached sectors.   */
} t_cache_const_t;

static uint8_t s_disk[(size_t)k_t_under_blocks * (size_t)k_ra8_io_block_size_bytes];
static uint8_t s_cache_data[(size_t)k_t_cache_slots * (size_t)k_ra8_io_block_size_bytes];
static ra8_io_blockdev_cache_slot_t s_cache_slots[(size_t)k_t_cache_slots];

/** @brief Fill block `b` of `bd` with byte `val`. @details Exercises the seed block path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] bd Block-device facade whose backing storage is seeded. @param[in] b Block index or data-buffer argument exercised by the helper. @param[in] val Byte value used to seed or verify the block fixture. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_seed_block(ra8_io_blockdev_t* bd, uint32_t b, uint8_t val)
{
  uint8_t blk[(size_t)k_ra8_io_block_size_bytes];
  (void)memset(blk, val, sizeof(blk));
  (void)ra8_io_blockdev_write(bd, b, 1, blk);
}

/** @brief True when every byte of a 512-byte block equals `val`. @details Exercises the block is path with bounded caller-owned fixture state and verifies its documented result. @param[in] blk Readable logical-block bytes. @param[in] val Byte value used to seed or verify the block fixture. @return Whether the named fixture condition holds. @retval true The named fixture condition holds. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static bool internal_block_is(const uint8_t* blk, uint8_t val)
{
  for (uint32_t i = 0; i < (uint32_t)k_ra8_io_block_size_bytes; ++i) {
    if (blk[i] != val) {
      return false;
    }
  }
  return true;
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a re-read hits, distinct reads miss, and
 * an LRU eviction forces a later miss; hit/miss counts are asserted) @brief Verify hit miss lru behavior. @details Executes the hit miss lru scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_hit_miss_lru(void)
{
  TEST_BEGIN("cache hit/miss + LRU");
  ra8_io_blockdev_ram_state_t ust   = {};
  ra8_io_blockdev_t           under = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_ram_init(&under, &ust, s_disk, k_t_under_blocks, false));
  internal_seed_block(&under, 0, 0x10);
  internal_seed_block(&under, 1, k_bdc_seed_block1);
  internal_seed_block(&under, 2, k_bdc_seed_block2);

  ra8_io_blockdev_cache_state_t cst = {};
  ra8_io_blockdev_t             cbd = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_blockdev_cache_init(&cbd, &cst, &under, s_cache_data, s_cache_slots, k_t_cache_slots));

  uint8_t blk[(size_t)k_ra8_io_block_size_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&cbd, 0, 1, blk)); /* miss */
  TEST_ASSERT(internal_block_is(blk, 0x10));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&cbd, 0, 1, blk)); /* hit            */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&cbd, 1, 1, blk)); /* miss           */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&cbd, 2, 1, blk)); /* miss, evicts 0 */
  TEST_ASSERT(internal_block_is(blk, 0x12));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&cbd, 0, 1, blk)); /* miss (0 evicted) */
  TEST_ASSERT(internal_block_is(blk, 0x10));

  uint32_t hits   = 0;
  uint32_t misses = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_cache_stats(&cst, &hits, &misses));
  TEST_ASSERT_EQ(1, hits);
  TEST_ASSERT_EQ(4, misses);
  TEST_END("cache hit/miss + LRU");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a cache write commits to the backend and
 * is served from the cache on the next read) @brief Verify write through behavior. @details Executes the write through scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_write_through(void)
{
  TEST_BEGIN("cache write-through");
  ra8_io_blockdev_ram_state_t ust   = {};
  ra8_io_blockdev_t           under = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_ram_init(&under, &ust, s_disk, k_t_under_blocks, false));
  ra8_io_blockdev_cache_state_t cst = {};
  ra8_io_blockdev_t             cbd = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_blockdev_cache_init(&cbd, &cst, &under, s_cache_data, s_cache_slots, k_t_cache_slots));

  uint8_t out[(size_t)k_ra8_io_block_size_bytes];
  (void)memset(out, k_cache_fill_poison, sizeof(out));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&cbd, 3, 1, out)); /* write-through */

  /* backend committed (read the underlying device directly) */
  uint8_t raw[(size_t)k_ra8_io_block_size_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&under, 3, 1, raw));
  TEST_ASSERT(internal_block_is(raw, 0x3C));

  /* the write cached block 3 -> next read through the cache is a hit */
  uint8_t blk[(size_t)k_ra8_io_block_size_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&cbd, 3, 1, blk));
  TEST_ASSERT(internal_block_is(blk, 0x3C));
  uint32_t hits = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_cache_stats(&cst, &hits, nullptr));
  TEST_ASSERT_EQ(1, hits);
  TEST_END("cache write-through");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- erasing a cached block invalidates it, so
 * the next read returns the erased backend contents) @brief Verify erase invalidate behavior. @details Executes the erase invalidate scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_erase_invalidate(void)
{
  TEST_BEGIN("cache erase invalidate");
  ra8_io_blockdev_ram_state_t ust   = {};
  ra8_io_blockdev_t           under = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_ram_init(&under, &ust, s_disk, k_t_under_blocks, false));
  internal_seed_block(&under, 4, k_bdc_seed_block4);
  ra8_io_blockdev_cache_state_t cst = {};
  ra8_io_blockdev_t             cbd = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_blockdev_cache_init(&cbd, &cst, &under, s_cache_data, s_cache_slots, k_t_cache_slots));

  uint8_t blk[(size_t)k_ra8_io_block_size_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&cbd, 4, 1, blk)); /* cache 0x44 */
  TEST_ASSERT(internal_block_is(blk, 0x44));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_erase(&cbd, 4, 1));     /* erase + invalidate       */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&cbd, 4, 1, blk)); /* re-read backend (zeroed) */
  TEST_ASSERT(internal_block_is(blk, 0x00));
  TEST_END("cache erase invalidate");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- init rejects each NULL argument and the
 * zero-slot case via independent single-condition guards) @brief Verify init validation behavior. @details Executes the init validation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_init_validation(void)
{
  TEST_BEGIN("cache init validation");
  ra8_io_blockdev_ram_state_t ust   = {};
  ra8_io_blockdev_t           under = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_ram_init(&under, &ust, s_disk, k_t_under_blocks, false));
  ra8_io_blockdev_cache_state_t cst = {};
  ra8_io_blockdev_t             cbd = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_blockdev_cache_init(nullptr,
                                            &cst,
                                            &under,
                                            s_cache_data,
                                            s_cache_slots,
                                            k_t_cache_slots));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_io_blockdev_cache_init(&cbd, &cst, nullptr, s_cache_data, s_cache_slots, k_t_cache_slots));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_io_blockdev_cache_init(&cbd, &cst, &under, s_cache_data, s_cache_slots, 0));
  TEST_END("cache init validation");
}

int32_t main(void)
{
  internal_test_hit_miss_lru();
  internal_test_write_through();
  internal_test_erase_invalidate();
  internal_test_init_validation();
  return 0;
}
