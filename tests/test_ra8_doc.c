/**
 * @file test_ra8_doc.c
 * @brief Unit tests for ra8_doc.c (Data Operation Circuit driver)
 *
 * @details
 * The RAM-backed host register file has no DOC operation engine, so
 * these tests assert the driver's real MMIO trace -- DOCR mode
 * programming, the DODSR0 seed write, the DODIR trigger write, and the
 * DODSR0 readback -- rather than the arithmetic, which the silicon
 * engine performs and the ``doc_demo`` HIL app proves on hardware.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_doc.h"
#include "ra8_doc_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

/**
 * @enum doc_fixture_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint8_t {
  k_doc_probe_docr =
    0xFFU, /**< Every DOCR bit set, so a write reaching a neighbouring register leaves evidence. */
} doc_fixture_t;

/**
 * @enum doc_fixture2_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint16_t {
  k_doc_probe_dodir = 0xAAAAU, /**< Alternating bits in DODIR. */
  k_doc_probe_dodsr0 =
    0x5555U, /**< Their complement in DODSR0, so swapping the two registers is unmistakable. */
} doc_fixture2_t;

typedef enum : uint16_t {
  k_ra8_doc_test_a = 0x1234U, /**< Operand A -- seeded into DODSR0. */
  k_ra8_doc_test_b = 0x00FFU, /**< Operand B -- written to DODIR.   */
} ra8_doc_test_value_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_clears_regs(void)
{
  TEST_BEGIN("doc init clears regs");
  ra8_fake_mmap_reset();

  volatile r_doc_regs_t* reg = ra8_doc();
  reg->DOCR                  = k_doc_probe_docr;
  reg->DODIR                 = k_doc_probe_dodir;
  reg->DODSR0                = k_doc_probe_dodsr0;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_init());
  TEST_ASSERT_EQ(0, reg->DOCR);
  TEST_ASSERT_EQ(0, reg->DODIR);
  TEST_ASSERT_EQ(0, reg->DODSR0);
  TEST_END("doc init clears regs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_add16_programs_trace(void)
{
  TEST_BEGIN("doc add16 programs the add-mode register trace");
  ra8_fake_mmap_reset();

  uint16_t sum = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_add16((uint16_t)k_ra8_doc_test_a, (uint16_t)k_ra8_doc_test_b, &sum));

  /* Mode bits OMS=01 should be visible in DOCR. */
  volatile r_doc_regs_t* reg = ra8_doc();
  TEST_ASSERT_EQ(k_ra8_doc_mode_add, (reg->DOCR & (uint8_t)k_ra8_doc_mask_oms));
  /* Operand B landed in DODIR (the trigger write). */
  volatile uint16_t* dodir = (volatile uint16_t*)&reg->DODIR;
  TEST_ASSERT_EQ(k_ra8_doc_test_b, *dodir);
  /* Host RAM has no operation engine: DODSR0 still holds the seed and
   * the readback returns it verbatim. The a+b arithmetic is silicon's
   * job, proven by the doc_demo HIL app. */
  volatile uint16_t* dodsr0 = (volatile uint16_t*)&reg->DODSR0;
  TEST_ASSERT_EQ(k_ra8_doc_test_a, *dodsr0);
  TEST_ASSERT_EQ(k_ra8_doc_test_a, sum);
  TEST_END("doc add16 programs the add-mode register trace");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_add16_null_out(void)
{
  TEST_BEGIN("doc add16 null out");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_doc_add16(1U, 2U, nullptr));
  TEST_END("doc add16 null out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sub16_programs_trace(void)
{
  TEST_BEGIN("doc sub16 programs the subtract-mode register trace");
  ra8_fake_mmap_reset();

  uint16_t diff = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_sub16((uint16_t)k_ra8_doc_test_a, (uint16_t)k_ra8_doc_test_b, &diff));

  /* Mode bits OMS=10 should be visible in DOCR. */
  volatile r_doc_regs_t* reg = ra8_doc();
  TEST_ASSERT_EQ(k_ra8_doc_mode_subtract, (reg->DOCR & (uint8_t)k_ra8_doc_mask_oms));
  /* Operand B landed in DODIR; DODSR0 holds the seed (no host engine). */
  volatile uint16_t* dodir = (volatile uint16_t*)&reg->DODIR;
  TEST_ASSERT_EQ(k_ra8_doc_test_b, *dodir);
  volatile uint16_t* dodsr0 = (volatile uint16_t*)&reg->DODSR0;
  TEST_ASSERT_EQ(k_ra8_doc_test_a, *dodsr0);
  TEST_ASSERT_EQ(k_ra8_doc_test_a, diff);
  TEST_END("doc sub16 programs the subtract-mode register trace");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sub16_null_out(void)
{
  TEST_BEGIN("doc sub16 null out");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_doc_sub16(10U, 4U, nullptr));
  TEST_END("doc sub16 null out");
}

int32_t main(void)
{
  test_init_clears_regs();
  test_add16_programs_trace();
  test_add16_null_out();
  test_sub16_programs_trace();
  test_sub16_null_out();
  return 0;
}
