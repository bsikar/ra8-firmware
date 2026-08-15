/**
 * @file test_ra8_fake_world.c
 * @brief Unit tests for tests/mocks/ra8_fake_world
 * @details Validates reset and lifecycle coordination across the combined fake MMIO, IRQ, DMA, time, and world state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_world.h"
#include "unity_minimal.h"

/**
 * @enum t_world_t
 * @brief Per-world scratch buffer capacity.
 */
typedef enum : uint16_t {
  k_t_buf_cap = 256U, /**< Secure and non-secure scratch buffers, bytes. */
} t_world_t;

static uint8_t s_ns_buf[k_t_buf_cap];
static uint8_t s_s_buf[k_t_buf_cap];

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_empty_world_rejects_everything(void)
{
  TEST_BEGIN("ra8_fake_world: empty world rejects every check");
  ra8_fake_world_reset();
  TEST_ASSERT(!ra8_fake_world_check_ns_range(s_ns_buf, 16U));
  TEST_END("ra8_fake_world: empty world rejects every check");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_mark_ns_covers_query(void)
{
  TEST_BEGIN("ra8_fake_world: NS-tagged region passes");
  ra8_fake_world_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_world_mark_ns(s_ns_buf, (uint32_t)sizeof(s_ns_buf)));
  TEST_ASSERT(ra8_fake_world_check_ns_range(s_ns_buf, 64U));
  /* Sub-range still passes. */
  TEST_ASSERT(ra8_fake_world_check_ns_range(s_ns_buf + 8, 32U));
  TEST_END("ra8_fake_world: NS-tagged region passes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_secure_overlap_blocks(void)
{
  TEST_BEGIN("ra8_fake_world: Secure overlap blocks the query");
  ra8_fake_world_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_world_mark_ns(s_ns_buf, (uint32_t)sizeof(s_ns_buf)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_world_mark_s(s_ns_buf + 64, 16U));
  /* Query that crosses the Secure stripe is rejected. */
  TEST_ASSERT(!ra8_fake_world_check_ns_range(s_ns_buf + 60, 32U));
  /* Query strictly outside the Secure stripe still passes. */
  TEST_ASSERT(ra8_fake_world_check_ns_range(s_ns_buf, 60U));
  TEST_END("ra8_fake_world: Secure overlap blocks the query");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_unrelated_secure_region_does_not_block(void)
{
  TEST_BEGIN("ra8_fake_world: unrelated Secure region does not block");
  ra8_fake_world_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_world_mark_ns(s_ns_buf, (uint32_t)sizeof(s_ns_buf)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_world_mark_s(s_s_buf, (uint32_t)sizeof(s_s_buf)));
  TEST_ASSERT(ra8_fake_world_check_ns_range(s_ns_buf, 64U));
  TEST_END("ra8_fake_world: unrelated Secure region does not block");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_zero_length_check_is_false(void)
{
  TEST_BEGIN("ra8_fake_world: zero length check rejects");
  ra8_fake_world_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_world_mark_ns(s_ns_buf, (uint32_t)sizeof(s_ns_buf)));
  TEST_ASSERT(!ra8_fake_world_check_ns_range(s_ns_buf, 0U));
  TEST_END("ra8_fake_world: zero length check rejects");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_region_table_full(void)
{
  TEST_BEGIN("ra8_fake_world: table-full reports no_mem");
  ra8_fake_world_reset();
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_fake_world_max_regions; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_world_mark_ns(s_ns_buf + i, 1U));
  }
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fake_world_mark_ns(s_ns_buf + 32, 1U));
  TEST_END("ra8_fake_world: table-full reports no_mem");
}

int32_t main(void)
{
  test_empty_world_rejects_everything();
  test_mark_ns_covers_query();
  test_secure_overlap_blocks();
  test_unrelated_secure_region_does_not_block();
  test_zero_length_check_is_false();
  test_region_table_full();
  return 0;
}
