/**
 * @file test_app_doc_demo.c
 * @brief Integration test: Data Operation Circuit (DOC) sum demo
 *
 * @details
 * Mirrors examples/ek_ra8d2/hw_validated/hil/doc_demo/src/main.c bring-up flow:
 * ra8_doc_init -> chained ra8_doc_add16 over the demo operand table.
 * The RAM-backed host register file has no DOC operation engine, so
 * the chained case asserts the driver's register trace (mode bits +
 * final DODIR operand) instead of the arithmetic; the hw-sum ==
 * sw-sum match is what the doc_demo HIL app proves on silicon.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_doc.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

/**
 * @brief Golden bring-up: ra8_doc_init succeeds.
 *
 * @par MC/DC:
 * Decision: ``ra8_doc_init != ok``. One atomic condition x 2
 * vectors -- success (this) + bad-pointer (covered below).
 */
static void test_doc_app_init_ok(void)
{
  reset_world();
  TEST_BEGIN("doc_demo: ra8_doc_init ok");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_init());
  TEST_END("doc_demo: ra8_doc_init ok");
}

/**
 * @brief One add16 round-trips through the DOC accumulator.
 *
 * @par MC/DC:
 * Decision: ``ra8_doc_add16 != ok``. One atomic condition x 2
 * vectors -- success (this) + NULL out_sum (below).
 */
static void test_doc_app_add_ok(void)
{
  reset_world();
  TEST_BEGIN("doc_demo: add16 round-trip");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_init());
  uint16_t sum = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_add16((uint16_t)0x1111U, (uint16_t)0x2222U, &sum));
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_init());
  TEST_ASSERT(ra8_doc_add16((uint16_t)0x1U, (uint16_t)0x2U, nullptr) != k_ra8_ok);
  TEST_END("doc_demo: add16 NULL rejected");
}

/**
 * @brief Chained add over the demo operand table drives the DOC trace.
 *
 * @par MC/DC:
 * Compound decision in app: ``hw_err != ok || hw_sum != sw_sum``.
 * Two atomic conditions x N+1 = 3 vectors -- hw_err ok every round
 * (this + the NULL-out_sum test above vary the error condition); the
 * hw/sw mismatch condition needs the silicon operation engine and is
 * exercised by the doc_demo HIL app (documented exception in the
 * demo's inline MC/DC block).
 */
static void test_doc_app_chained_trace(void)
{
  reset_world();
  TEST_BEGIN("doc_demo: chained add16 drives the DOC register trace");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_init());
  const uint16_t operands[] =
    {0x1111U, 0x2222U, 0x3333U, 0x0F0FU, 0xF0F0U, 0x00FFU, 0xFF00U, 0xDEADU};
  uint16_t acc = operands[0];
  for (uint8_t i = 1U; i < 8U; ++i) {
    uint16_t partial = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_add16(acc, operands[i], &partial));
    acc = partial;
  }
  /* Host RAM has no operation engine: every round returns its seed, so
   * the accumulator sticks at operands[0]. Assert the register trace
   * instead -- add mode latched and the final operand in DODIR; the
   * hw-sum == sw-sum match runs on silicon (doc_demo HIL). */
  TEST_ASSERT_EQ(operands[0], acc);
  volatile r_doc_regs_t* reg = ra8_doc();
  TEST_ASSERT_EQ(k_ra8_doc_mode_add, (reg->DOCR & (uint8_t)k_ra8_doc_mask_oms));
  volatile uint16_t* dodir = (volatile uint16_t*)&reg->DODIR;
  TEST_ASSERT_EQ(operands[7], *dodir);
  TEST_END("doc_demo: chained add16 drives the DOC register trace");
}

int main(void)
{
  test_doc_app_init_ok();
  test_doc_app_add_ok();
  test_doc_app_add_null();
  test_doc_app_chained_trace();
  return 0;
}
