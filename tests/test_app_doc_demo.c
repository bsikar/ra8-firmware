/**
 * @file test_app_doc_demo.c
 * @brief Integration test: Data Operation Circuit (DOC) sum demo
 *
 * @details
 * Mirrors examples/ek_ra8d2/doc_demo/main.c bring-up flow:
 * ra_doc_init -> chained ra_doc_add16 -> match against software
 * reference. Host shim returns deterministic add results so the
 * golden path always matches.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_doc.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static void reset_world(void)
{
  ra_sim_mmap_reset();
}

/**
 * @brief Golden bring-up: ra_doc_init succeeds.
 *
 * @par MC/DC:
 * Decision: ``ra_doc_init != ok``. One atomic condition x 2
 * vectors -- success (this) + bad-pointer (covered below).
 */
static void test_doc_app_init_ok(void)
{
  reset_world();
  TEST_BEGIN("doc_demo: ra_doc_init ok");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_doc_init());
  TEST_END("doc_demo: ra_doc_init ok");
}

/**
 * @brief One add16 round-trips through the DOC accumulator.
 *
 * @par MC/DC:
 * Decision: ``ra_doc_add16 != ok``. One atomic condition x 2
 * vectors -- success (this) + NULL out_sum (below).
 */
static void test_doc_app_add_ok(void)
{
  reset_world();
  TEST_BEGIN("doc_demo: add16 round-trip");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_doc_init());
  uint16_t sum = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_doc_add16((uint16_t)0x1111U, (uint16_t)0x2222U, &sum));
  TEST_END("doc_demo: add16 round-trip");
}

/**
 * @brief NULL out_sum rejected by add16.
 *
 * @par MC/DC:
 * Decision: ``out_sum == nullptr``. One atomic condition x 2
 * vectors -- non-NULL above + this NULL.
 */
static void test_doc_app_add_null(void)
{
  reset_world();
  TEST_BEGIN("doc_demo: add16 NULL rejected");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_doc_init());
  TEST_ASSERT(ra_doc_add16((uint16_t)0x1U, (uint16_t)0x2U, nullptr) != k_ra_ok);
  TEST_END("doc_demo: add16 NULL rejected");
}

/**
 * @brief Chained add of the demo operand table matches software ref.
 *
 * @par MC/DC:
 * Compound decision in app: ``hw_err != ok || hw_sum != sw_sum``.
 * Two atomic conditions x N+1 = 3 vectors -- both-ok-match (this),
 * hw_err (covered by NULL-out_sum test above), mismatch (cannot be
 * driven from the golden mock; documented exception in the demo's
 * inline MC/DC block).
 */
static void test_doc_app_chained_match(void)
{
  reset_world();
  TEST_BEGIN("doc_demo: chained sum matches software");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_doc_init());
  const uint16_t operands[] =
    {0x1111U, 0x2222U, 0x3333U, 0x0F0FU, 0xF0F0U, 0x00FFU, 0xFF00U, 0xDEADU};
  uint16_t acc = operands[0];
  for (uint8_t i = 1U; i < 8U; ++i) {
    uint16_t partial = 0U;
    TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_doc_add16(acc, operands[i], &partial));
    acc = partial;
  }
  uint16_t sw = 0U;
  for (uint8_t i = 0U; i < 8U; ++i) {
    sw = (uint16_t)(sw + operands[i]);
  }
  TEST_ASSERT_EQ((int)sw, (int)acc);
  TEST_END("doc_demo: chained sum matches software");
}

int main(void)
{
  test_doc_app_init_ok();
  test_doc_app_add_ok();
  test_doc_app_add_null();
  test_doc_app_chained_match();
  return 0;
}
