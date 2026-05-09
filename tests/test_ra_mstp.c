/**
 * @file test_ra_mstp.c
 * @brief Unit tests for the ref-counted MSTP wrapper (libs/ra_hal/src/ra_mstp.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_mstp_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/* Helper -- read the raw bit value for an id from the simulator
 * MMIO so the test can independently confirm what ra_mstp wrote. */
static bool peek_bit(ra_mstp_t id)
{
  const uint8_t            reg  = (uint8_t)ra_mstp_id_reg(id);
  const uint8_t            bit  = ra_mstp_id_bit(id);
  volatile const uint32_t* base = &ra_mstp()->MSTPCRA;
  return ((base[reg] & ((uint32_t)1U << bit)) != 0U);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_zeroes_refcounts_and_sets_all_stopped(void)
{
  TEST_BEGIN("ra_mstp_init -> all stopped, zero refcounts");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_init());

  /* Every register should now be all-ones. */
  TEST_ASSERT_EQ(0xFFFFFFFFU, ra_mstp()->MSTPCRA);
  TEST_ASSERT_EQ(0xFFFFFFFFU, ra_mstp()->MSTPCRB);
  TEST_ASSERT_EQ(0xFFFFFFFFU, ra_mstp()->MSTPCRC);
  TEST_ASSERT_EQ(0xFFFFFFFFU, ra_mstp()->MSTPCRD);
  TEST_ASSERT_EQ(0xFFFFFFFFU, ra_mstp()->MSTPCRE);

  /* Spot-check a few refcounts. */
  uint8_t ref = 0xFFU;
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_get_refcount(k_ra_mstp_sci0, &ref));
  TEST_ASSERT_EQ(0, ref);
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_get_refcount(k_ra_mstp_dmac0_dtc0, &ref));
  TEST_ASSERT_EQ(0, ref);

  TEST_END("ra_mstp_init -> all stopped, zero refcounts");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_clears_bit_first_request(void)
{
  TEST_BEGIN("ra_mstp_enable: first request clears bit");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_init());
  TEST_ASSERT(peek_bit(k_ra_mstp_sci0)); /* stopped */

  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_enable(k_ra_mstp_sci0));
  TEST_ASSERT(!peek_bit(k_ra_mstp_sci0)); /* running */

  uint8_t ref = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_get_refcount(k_ra_mstp_sci0, &ref));
  TEST_ASSERT_EQ(1, ref);

  TEST_END("ra_mstp_enable: first request clears bit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_idempotent_increments_refcount(void)
{
  TEST_BEGIN("ra_mstp_enable: second request increments refcount only");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_enable(k_ra_mstp_dmac0_dtc0));
  TEST_ASSERT(!peek_bit(k_ra_mstp_dmac0_dtc0));

  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_enable(k_ra_mstp_dmac0_dtc0));
  TEST_ASSERT(!peek_bit(k_ra_mstp_dmac0_dtc0));

  uint8_t ref = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_get_refcount(k_ra_mstp_dmac0_dtc0, &ref));
  TEST_ASSERT_EQ(2, ref);

  TEST_END("ra_mstp_enable: second request increments refcount only");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_disable_keeps_bit_clear_until_last_release(void)
{
  TEST_BEGIN("ra_mstp_disable: bit stays clear until refcount hits 0");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_enable(k_ra_mstp_dmac0_dtc0));
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_enable(k_ra_mstp_dmac0_dtc0));

  /* First disable: refcount drops to 1 but the bit stays clear. */
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_disable(k_ra_mstp_dmac0_dtc0));
  TEST_ASSERT(!peek_bit(k_ra_mstp_dmac0_dtc0));

  /* Second disable: refcount hits 0, bit goes back to stopped. */
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_disable(k_ra_mstp_dmac0_dtc0));
  TEST_ASSERT(peek_bit(k_ra_mstp_dmac0_dtc0));

  uint8_t ref = 0xFFU;
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_get_refcount(k_ra_mstp_dmac0_dtc0, &ref));
  TEST_ASSERT_EQ(0, ref);

  TEST_END("ra_mstp_disable: bit stays clear until refcount hits 0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_disable_underflow_returns_invalid_state(void)
{
  TEST_BEGIN("ra_mstp_disable: underflow rejected");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_init());
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_mstp_disable(k_ra_mstp_sci0));

  TEST_END("ra_mstp_disable: underflow rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_invalid_id_rejected(void)
{
  TEST_BEGIN("ra_mstp_enable / disable / get_refcount: invalid id");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_init());

  /* reg=7, bit=0 -> out of range. */
  const ra_mstp_t bogus = (ra_mstp_t)((7U << 8) | 0U);

  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_mstp_enable(bogus));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_mstp_disable(bogus));

  uint8_t ref = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_mstp_get_refcount(bogus, &ref));

  /* reg=0, bit=33 -> out of range. */
  const ra_mstp_t bogus2 = (ra_mstp_t)((0U << 8) | 33U);
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_mstp_enable(bogus2));

  TEST_END("ra_mstp_enable / disable / get_refcount: invalid id");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_refcount_null_out(void)
{
  TEST_BEGIN("ra_mstp_get_refcount: NULL out_ref");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_mstp_get_refcount(k_ra_mstp_sci0, nullptr));
  TEST_END("ra_mstp_get_refcount: NULL out_ref");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_is_stopped_reads_bit(void)
{
  TEST_BEGIN("ra_mstp_is_stopped: tracks live bit");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_init());

  bool stopped = false;
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_is_stopped(k_ra_mstp_iic0, &stopped));
  TEST_ASSERT(stopped);

  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_enable(k_ra_mstp_iic0));
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_is_stopped(k_ra_mstp_iic0, &stopped));
  TEST_ASSERT(!stopped);

  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_mstp_is_stopped(k_ra_mstp_iic0, nullptr));

  TEST_END("ra_mstp_is_stopped: tracks live bit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_neighbor_bits_undisturbed(void)
{
  TEST_BEGIN("ra_mstp_enable: neighbor bits unchanged");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_init());

  /* Pre-state: every bit set in MSTPCRB. */
  const uint32_t before = ra_mstp()->MSTPCRB;

  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_enable(k_ra_mstp_sci3));

  /* Bit 28 (SCI3) should now be 0 but every other bit is unchanged. */
  const uint32_t after    = ra_mstp()->MSTPCRB;
  const uint32_t expected = before & ~((uint32_t)1U << 28);
  TEST_ASSERT_EQ(expected, after);

  TEST_END("ra_mstp_enable: neighbor bits unchanged");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_all_five_registers_addressable(void)
{
  TEST_BEGIN("ra_mstp_enable: covers MSTPCRA..MSTPCRE");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_init());

  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_enable(k_ra_mstp_sram0));  /* A */
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_enable(k_ra_mstp_sci0));   /* B */
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_enable(k_ra_mstp_crc));    /* C */
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_enable(k_ra_mstp_adc16h)); /* D */
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_enable(k_ra_mstp_gpt0));   /* E */

  TEST_ASSERT(!peek_bit(k_ra_mstp_sram0));
  TEST_ASSERT(!peek_bit(k_ra_mstp_sci0));
  TEST_ASSERT(!peek_bit(k_ra_mstp_crc));
  TEST_ASSERT(!peek_bit(k_ra_mstp_adc16h));
  TEST_ASSERT(!peek_bit(k_ra_mstp_gpt0));

  TEST_END("ra_mstp_enable: covers MSTPCRA..MSTPCRE");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_refcount_saturation(void)
{
  TEST_BEGIN("ra_mstp_enable: refcount saturation rejected");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_init());

  /* Push the refcount up to UINT8_MAX (255). */
  for (uint16_t i = 0U; i < 255U; ++i) {
    TEST_ASSERT_EQ(k_ra_ok, ra_mstp_enable(k_ra_mstp_dmac0_dtc0));
  }
  uint8_t ref = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_get_refcount(k_ra_mstp_dmac0_dtc0, &ref));
  TEST_ASSERT_EQ(255, ref);

  /* The 256th request must be rejected without changing state. */
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_mstp_enable(k_ra_mstp_dmac0_dtc0));
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_get_refcount(k_ra_mstp_dmac0_dtc0, &ref));
  TEST_ASSERT_EQ(255, ref);

  TEST_END("ra_mstp_enable: refcount saturation rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_is_stopped_invalid_id(void)
{
  TEST_BEGIN("ra_mstp_is_stopped: invalid id rejected");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_init());

  bool stopped = false;
  /* reg=9 -> out of range. */
  const ra_mstp_t bogus = (ra_mstp_t)((9U << 8) | 0U);
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_mstp_is_stopped(bogus, &stopped));

  TEST_END("ra_mstp_is_stopped: invalid id rejected");
}

int32_t main(void)
{
  test_init_zeroes_refcounts_and_sets_all_stopped();
  test_enable_clears_bit_first_request();
  test_enable_idempotent_increments_refcount();
  test_disable_keeps_bit_clear_until_last_release();
  test_disable_underflow_returns_invalid_state();
  test_invalid_id_rejected();
  test_get_refcount_null_out();
  test_is_stopped_reads_bit();
  test_neighbor_bits_undisturbed();
  test_all_five_registers_addressable();
  test_refcount_saturation();
  test_is_stopped_invalid_id();
  (void)fprintf(stderr, "[OK  ] test_ra_mstp.c\n");
  return 0;
}
