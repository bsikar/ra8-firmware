/**
 * @file test_ra8_sim_world.c
 * @brief Unit tests for tests/mocks/ra8_sim_world
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_sim_world.h"
#include "unity_minimal.h"

/**
 * @enum sim_world_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_sim_world_val_256 = 256,
} sim_world_uint16_const_t;

static uint8_t s_ns_buf[k_sim_world_val_256];
static uint8_t s_s_buf[k_sim_world_val_256];

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_empty_world_rejects_everything(void)
{
  TEST_BEGIN("ra8_sim_world: empty world rejects every check");
  ra8_sim_world_reset();
  TEST_ASSERT(!ra8_sim_world_check_ns_range(s_ns_buf, 16U));
  TEST_END("ra8_sim_world: empty world rejects every check");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_mark_ns_covers_query(void)
{
  TEST_BEGIN("ra8_sim_world: NS-tagged region passes");
  ra8_sim_world_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_world_mark_ns(s_ns_buf, (uint32_t)sizeof(s_ns_buf)));
  TEST_ASSERT(ra8_sim_world_check_ns_range(s_ns_buf, 64U));
  /* Sub-range still passes. */
  TEST_ASSERT(ra8_sim_world_check_ns_range(s_ns_buf + 8, 32U));
  TEST_END("ra8_sim_world: NS-tagged region passes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_secure_overlap_blocks(void)
{
  TEST_BEGIN("ra8_sim_world: Secure overlap blocks the query");
  ra8_sim_world_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_world_mark_ns(s_ns_buf, (uint32_t)sizeof(s_ns_buf)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_world_mark_s(s_ns_buf + 64, 16U));
  /* Query that crosses the Secure stripe is rejected. */
  TEST_ASSERT(!ra8_sim_world_check_ns_range(s_ns_buf + 60, 32U));
  /* Query strictly outside the Secure stripe still passes. */
  TEST_ASSERT(ra8_sim_world_check_ns_range(s_ns_buf, 60U));
  TEST_END("ra8_sim_world: Secure overlap blocks the query");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_unrelated_secure_region_does_not_block(void)
{
  TEST_BEGIN("ra8_sim_world: unrelated Secure region does not block");
  ra8_sim_world_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_world_mark_ns(s_ns_buf, (uint32_t)sizeof(s_ns_buf)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_world_mark_s(s_s_buf, (uint32_t)sizeof(s_s_buf)));
  TEST_ASSERT(ra8_sim_world_check_ns_range(s_ns_buf, 64U));
  TEST_END("ra8_sim_world: unrelated Secure region does not block");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_zero_length_check_is_false(void)
{
  TEST_BEGIN("ra8_sim_world: zero length check rejects");
  ra8_sim_world_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_world_mark_ns(s_ns_buf, (uint32_t)sizeof(s_ns_buf)));
  TEST_ASSERT(!ra8_sim_world_check_ns_range(s_ns_buf, 0U));
  TEST_END("ra8_sim_world: zero length check rejects");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_region_table_full(void)
{
  TEST_BEGIN("ra8_sim_world: table-full reports no_mem");
  ra8_sim_world_reset();
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_sim_world_max_regions; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_world_mark_ns(s_ns_buf + i, 1U));
  }
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_sim_world_mark_ns(s_ns_buf + 32, 1U));
  TEST_END("ra8_sim_world: table-full reports no_mem");
}

int32_t main(void)
{
  test_empty_world_rejects_everything();
  test_mark_ns_covers_query();
  test_secure_overlap_blocks();
  test_unrelated_secure_region_does_not_block();
  test_zero_length_check_is_false();
  test_region_table_full();
  (void)fprintf(stderr, "[OK ] test_ra8_sim_world.c\n");
  return 0;
}
