/**
 * @file test_ra8_mstp.c
 * @brief Unit tests for the ref-counted MSTP wrapper (libs/ra8_hal/src/ra8_mstp.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_mstp_internal.h"
#include "ra8_mstp_regs.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "unity_minimal.h"

/**
 * @enum t_mstp_t
 * @brief Module-stop sweep bound and the reference-count seed.
 */
typedef enum : uint16_t {
  k_t_module_sweep = 255U,  /**< Module ids swept: past every defined module, so
                                 the undefined ones must be rejected.           */
  k_t_ref_unset    = 0xFFU, /**< Pre-set reference count; a query that fails
                                 must leave it rather than report zero.          */
} t_mstp_t;

/* Helper -- read the raw bit value for an id from the simulator
 * MMIO so the test can independently confirm what ra8_mstp wrote. */
static bool peek_bit(ra8_mstp_t id)
{
  const uint8_t            reg  = (uint8_t)ra8_mstp_id_reg(id);
  const uint8_t            bit  = ra8_mstp_id_bit(id);
  volatile const uint32_t* base = &ra8_mstp()->MSTPCRA;
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
  TEST_BEGIN("ra8_mstp_init -> all stopped, zero refcounts");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());

  /* MSTPCRA bits 0-3 (SRAM0-3) must stay 0 to keep SRAM running. */
  TEST_ASSERT_EQ(0xFFFFFFF0U, ra8_mstp()->MSTPCRA);
  TEST_ASSERT_EQ(0xFFFFFFFFU, ra8_mstp()->MSTPCRB);
  TEST_ASSERT_EQ(0xFFFFFFFFU, ra8_mstp()->MSTPCRC);
  TEST_ASSERT_EQ(0xFFFFFFFFU, ra8_mstp()->MSTPCRD);
  TEST_ASSERT_EQ(0xFFFFFFFFU, ra8_mstp()->MSTPCRE);

  /* Spot-check a few refcounts. */
  uint8_t ref = k_t_ref_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_get_refcount(k_ra8_mstp_sci0, &ref));
  TEST_ASSERT_EQ(0, ref);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_get_refcount(k_ra8_mstp_dmac0_dtc0, &ref));
  TEST_ASSERT_EQ(0, ref);

  TEST_END("ra8_mstp_init -> all stopped, zero refcounts");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_clears_bit_first_request(void)
{
  TEST_BEGIN("ra8_mstp_enable: first request clears bit");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT(peek_bit(k_ra8_mstp_sci0)); /* stopped */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_sci0));
  TEST_ASSERT(!peek_bit(k_ra8_mstp_sci0)); /* running */

  uint8_t ref = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_get_refcount(k_ra8_mstp_sci0, &ref));
  TEST_ASSERT_EQ(1, ref);

  TEST_END("ra8_mstp_enable: first request clears bit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_idempotent_increments_refcount(void)
{
  TEST_BEGIN("ra8_mstp_enable: second request increments refcount only");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_dmac0_dtc0));
  TEST_ASSERT(!peek_bit(k_ra8_mstp_dmac0_dtc0));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_dmac0_dtc0));
  TEST_ASSERT(!peek_bit(k_ra8_mstp_dmac0_dtc0));

  uint8_t ref = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_get_refcount(k_ra8_mstp_dmac0_dtc0, &ref));
  TEST_ASSERT_EQ(2, ref);

  TEST_END("ra8_mstp_enable: second request increments refcount only");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_disable_keeps_bit_clear_until_last_release(void)
{
  TEST_BEGIN("ra8_mstp_disable: bit stays clear until refcount hits 0");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_dmac0_dtc0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_dmac0_dtc0));

  /* First disable: refcount drops to 1 but the bit stays clear. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_disable(k_ra8_mstp_dmac0_dtc0));
  TEST_ASSERT(!peek_bit(k_ra8_mstp_dmac0_dtc0));

  /* Second disable: refcount hits 0, bit goes back to stopped. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_disable(k_ra8_mstp_dmac0_dtc0));
  TEST_ASSERT(peek_bit(k_ra8_mstp_dmac0_dtc0));

  uint8_t ref = k_t_ref_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_get_refcount(k_ra8_mstp_dmac0_dtc0, &ref));
  TEST_ASSERT_EQ(0, ref);

  TEST_END("ra8_mstp_disable: bit stays clear until refcount hits 0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_disable_underflow_returns_invalid_state(void)
{
  TEST_BEGIN("ra8_mstp_disable: underflow rejected");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_mstp_disable(k_ra8_mstp_sci0));

  TEST_END("ra8_mstp_disable: underflow rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_invalid_id_rejected(void)
{
  TEST_BEGIN("ra8_mstp_enable / disable / get_refcount: invalid id");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());

  /* reg=7, bit=0 -> out of range. */
  const ra8_mstp_t bogus = (ra8_mstp_t)((7U << 8) | 0U);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mstp_enable(bogus));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mstp_disable(bogus));

  uint8_t ref = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mstp_get_refcount(bogus, &ref));

  /* reg=0, bit=33 -> out of range. */
  const ra8_mstp_t bogus2 = (ra8_mstp_t)((0U << 8) | 33U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mstp_enable(bogus2));

  TEST_END("ra8_mstp_enable / disable / get_refcount: invalid id");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_refcount_null_out(void)
{
  TEST_BEGIN("ra8_mstp_get_refcount: NULL out_ref");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mstp_get_refcount(k_ra8_mstp_sci0, nullptr));
  TEST_END("ra8_mstp_get_refcount: NULL out_ref");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_is_stopped_reads_bit(void)
{
  TEST_BEGIN("ra8_mstp_is_stopped: tracks live bit");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());

  bool stopped = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_is_stopped(k_ra8_mstp_iic0, &stopped));
  TEST_ASSERT(stopped);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_iic0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_is_stopped(k_ra8_mstp_iic0, &stopped));
  TEST_ASSERT(!stopped);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mstp_is_stopped(k_ra8_mstp_iic0, nullptr));

  TEST_END("ra8_mstp_is_stopped: tracks live bit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_neighbor_bits_undisturbed(void)
{
  TEST_BEGIN("ra8_mstp_enable: neighbor bits unchanged");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());

  /* Pre-state: every bit set in MSTPCRB. */
  const uint32_t before = ra8_mstp()->MSTPCRB;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_sci3));

  /* Bit 28 (SCI3) should now be 0 but every other bit is unchanged. */
  const uint32_t after    = ra8_mstp()->MSTPCRB;
  const uint32_t expected = before & ~((uint32_t)1U << 28);
  TEST_ASSERT_EQ(expected, after);

  TEST_END("ra8_mstp_enable: neighbor bits unchanged");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_all_five_registers_addressable(void)
{
  TEST_BEGIN("ra8_mstp_enable: covers MSTPCRA..MSTPCRE");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_sram0));  /* A */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_sci0));   /* B */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_crc));    /* C */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_adc16h)); /* D */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_gpt0));   /* E */

  TEST_ASSERT(!peek_bit(k_ra8_mstp_sram0));
  TEST_ASSERT(!peek_bit(k_ra8_mstp_sci0));
  TEST_ASSERT(!peek_bit(k_ra8_mstp_crc));
  TEST_ASSERT(!peek_bit(k_ra8_mstp_adc16h));
  TEST_ASSERT(!peek_bit(k_ra8_mstp_gpt0));

  TEST_END("ra8_mstp_enable: covers MSTPCRA..MSTPCRE");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_refcount_saturation(void)
{
  TEST_BEGIN("ra8_mstp_enable: refcount saturation rejected");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());

  /* Push the refcount up to UINT8_MAX (255). */
  for (uint16_t i = 0U; i < k_t_module_sweep; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_dmac0_dtc0));
  }
  uint8_t ref = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_get_refcount(k_ra8_mstp_dmac0_dtc0, &ref));
  TEST_ASSERT_EQ(255, ref);

  /* The 256th request must be rejected without changing state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_mstp_enable(k_ra8_mstp_dmac0_dtc0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_get_refcount(k_ra8_mstp_dmac0_dtc0, &ref));
  TEST_ASSERT_EQ(255, ref);

  TEST_END("ra8_mstp_enable: refcount saturation rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_is_stopped_invalid_id(void)
{
  TEST_BEGIN("ra8_mstp_is_stopped: invalid id rejected");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());

  bool stopped = false;
  /* reg=9 -> out of range. */
  const ra8_mstp_t bogus = (ra8_mstp_t)((9U << 8) | 0U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mstp_is_stopped(bogus, &stopped));

  TEST_END("ra8_mstp_is_stopped: invalid id rejected");
}

/**
 * @test test_enable_readback_timeout_rolls_back
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the readback poll in
 * ``internal_wait_readback`` is a single-condition loop exit; the
 * armed fail drives the enable-side timeout leg and the satisfy-after
 * re-arm drives the loop-continuation leg, each in isolation)
 */
static void test_enable_readback_timeout_rolls_back(void)
{
  TEST_BEGIN("ra8_mstp_enable: readback timeout rolls the refcount back");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());

  /* Arm the seam on MSTPCRB (SCI0's register) so the ungate readback
   * never settles: enable must report hw_timeout and roll the count
   * back so a caller retry starts fresh. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait(&ra8_mstp()->MSTPCRB));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_mstp_enable(k_ra8_mstp_sci0));
  uint8_t ref = k_t_ref_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_get_refcount(k_ra8_mstp_sci0, &ref));
  TEST_ASSERT_EQ(0U, ref);

  /* Retry leg: the readback settles on its third poll and the enable
   * completes, proving the loop-continuation branch. */
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_satisfy_after(&ra8_mstp()->MSTPCRB, 2U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_sci0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_get_refcount(k_ra8_mstp_sci0, &ref));
  TEST_ASSERT_EQ(1U, ref);

  ra8_sim_mmio_reset();
  TEST_END("ra8_mstp_enable: readback timeout rolls the refcount back");
}

/**
 * @test test_disable_readback_timeout_rolls_back
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the readback poll in
 * ``internal_wait_readback`` is a single-condition loop exit; the
 * armed fail drives the disable-side timeout leg)
 */
static void test_disable_readback_timeout_rolls_back(void)
{
  TEST_BEGIN("ra8_mstp_disable: readback timeout keeps the module owned");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_sci0));

  /* The gate readback never settles: disable must report hw_timeout
   * and keep the refcount at 1 so ownership is not lost. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait(&ra8_mstp()->MSTPCRB));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_mstp_disable(k_ra8_mstp_sci0));
  uint8_t ref = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_get_refcount(k_ra8_mstp_sci0, &ref));
  TEST_ASSERT_EQ(1U, ref);

  ra8_sim_mmio_reset();
  TEST_END("ra8_mstp_disable: readback timeout keeps the module owned");
}

/**
 * @test test_init_readback_timeout
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the init readback poll is a
 * single-condition loop exit per MSTPCR register; failing MSTPCRE
 * proves the per-register loop iterated past the first four before
 * surfacing the timeout)
 */
static void test_init_readback_timeout(void)
{
  TEST_BEGIN("ra8_mstp_init: readback timeout on the last register surfaces");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();

  /* Fail the LAST register's readback so MSTPCRA..MSTPCRD settle first:
   * the init loop must iterate through all five before reporting. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait(&ra8_mstp()->MSTPCRE));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_mstp_init());

  ra8_sim_mmio_reset();
  TEST_END("ra8_mstp_init: readback timeout on the last register surfaces");
}

/**
 * @enum mstp_psar_test_t
 * @brief PSAR addresses + the USB Non-secure attribution mask for the tests.
 */
typedef enum : uintptr_t {
  k_t_psarb_addr = 0x40204004U, /**< PSARB (MSTPCRB attribution). */
} mstp_psar_test_t;

typedef enum : uint32_t {
  k_t_psarb_usb_ns = 0x00001800U, /**< PSARB11|12: USBFS0 + USBHS Non-secure.       */
  k_t_mstpb_stuck  = 0xFFFFE7FFU, /**< MSTPCRB read-back with bits 11,12 stuck low. */
  k_t_mstpb_all    = 0xFFFFFFFFU, /**< MSTPCRB all-stopped commanded value.         */
} mstp_mask_val_t;

/**
 * @test test_mstp_ns_mask_reads_psar
 * @brief The Non-secure mask is the PSAR value for B..E and 0 for A.
 *
 * @par MC/DC:
 * Decision: `k_psar_addr[reg] == 0U` in
 * libs/ra8_hal/src/ra8_mstp.c@ra8_mstp_ns_mask_internal (1 condition):
 * - reg 0 (MSTPCRA, no PSAR) -> true  -> mask 0.
 * - reg 1 (MSTPCRB, has PSAR) -> false -> returns the PSARB value.
 * The two vectors drive both arms; N+1 = 2 for N = 1.
 */
static void test_mstp_ns_mask_reads_psar(void)
{
  TEST_BEGIN("ra8_mstp_ns_mask_internal: reads PSAR, 0 for MSTPCRA");
  ra8_sim_mmap_reset();

  /* MSTPCRA has no attribution register -> always 0 (fully Secure-owned). */
  TEST_ASSERT_EQ(0U, ra8_mstp_ns_mask_internal((uint8_t)k_ra8_mstp_reg_a));

  /* MSTPCRB reflects PSARB: mark both USB controllers Non-secure. */
  *(volatile uint32_t*)k_t_psarb_addr = (uint32_t)k_t_psarb_usb_ns;
  TEST_ASSERT_EQ(k_t_psarb_usb_ns, ra8_mstp_ns_mask_internal((uint8_t)k_ra8_mstp_reg_b));

  /* A cleared PSAR (non-TrustZone) yields an empty mask -> strict read-back. */
  *(volatile uint32_t*)k_t_psarb_addr = 0U;
  TEST_ASSERT_EQ(0U, ra8_mstp_ns_mask_internal((uint8_t)k_ra8_mstp_reg_b));

  TEST_END("ra8_mstp_ns_mask_internal: reads PSAR, 0 for MSTPCRA");
}

/**
 * @test test_mstp_init_passes_with_ns_usb
 * @brief End-to-end: ra8_mstp_init succeeds when USB is Non-secure-attributed.
 *
 * @par MC/DC:
 * (no compound decisions under test -- integration over the public init with a
 * non-zero PSARB; the masked-equality decision itself is covered by
 * test_mstp_readback_mask_tolerates_ns_bits)
 */
static void test_mstp_init_passes_with_ns_usb(void)
{
  TEST_BEGIN("ra8_mstp_init: succeeds with USB delegated Non-secure");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();

  /* Mark both USB controllers Non-secure before the substrate init runs. */
  *(volatile uint32_t*)k_t_psarb_addr = (uint32_t)k_t_psarb_usb_ns;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());

  *(volatile uint32_t*)k_t_psarb_addr = 0U;
  TEST_END("ra8_mstp_init: succeeds with USB delegated Non-secure");
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
  test_enable_readback_timeout_rolls_back();
  test_disable_readback_timeout_rolls_back();
  test_init_readback_timeout();
  test_mstp_ns_mask_reads_psar();
  test_mstp_init_passes_with_ns_usb();
  (void)fprintf(stderr, "[OK  ] test_ra8_mstp.c\n");
  return 0;
}
