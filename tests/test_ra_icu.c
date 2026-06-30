/**
 * @file test_ra_icu.c
 * @brief Unit tests for ra_icu.c (Interrupt Control Unit)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_icu_regs.h"
#include "ra_err.h"
#include "ra_icu.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_clears_irqcr_and_nmi(void)
{
  TEST_BEGIN("ra_icu_init: IRQCR + NMIER + WUPEN cleared");
  ra_sim_mmap_reset();

  /* Pre-pollute IRQCR0, IRQCR16 (IRQCRb[0]), NMIER, WUPEN0, WUPEN1 so
   * we can observe the clear. */
  *ra_icu_irqcr(0U)  = 0xFFU;
  *ra_icu_irqcr(16U) = 0xFFU;
  *ra_icu_nmier()    = 0x1FFFFFUL;
  *ra_icu_wupen0()   = 0xCAFEBABEUL;
  *ra_icu_wupen1()   = 0xDEADBEEFUL;

  TEST_ASSERT_EQ(k_ra_ok, ra_icu_init());
  TEST_ASSERT_EQ(0, *ra_icu_irqcr(0U));
  TEST_ASSERT_EQ(0, *ra_icu_irqcr(16U));
  TEST_ASSERT_EQ(0UL, *ra_icu_nmier());
  TEST_ASSERT_EQ(0UL, *ra_icu_wupen0());
  TEST_ASSERT_EQ(0UL, *ra_icu_wupen1());
  TEST_END("ra_icu_init: IRQCR + NMIER + WUPEN cleared");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ielsr_mask_is_10_bits(void)
{
  TEST_BEGIN("ielsr IELS mask is 10 bits (FSP cross-check)");
  /* RA8D2 FSP R_ICU_Type IELSR_b shows IELS : 10 at [9:0].  Make sure
   * our mask includes bit 9. */
  TEST_ASSERT_EQ(0x3FF, k_ra_ielsr_iels_mask);
  TEST_END("ielsr IELS mask is 10 bits (FSP cross-check)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_irq_pin_irqcrb(void)
{
  TEST_BEGIN("ra_icu_configure_irq_pin: IRQCRb channel 20");
  ra_sim_mmap_reset();
  (void)ra_icu_init();

  const ra_icu_irq_cfg_t cfg = {
    .sense      = k_ra_icu_irqmd_both,
    .filter_div = k_ra_icu_fclksel_pclkb_64,
    .filter_en  = true,
  };
  /* Channel 20 (IRQCRb[4]) lives at base + 0x14 + 4 = base + 0x18. */
  TEST_ASSERT_EQ(k_ra_ok, ra_icu_configure_irq_pin(20U, &cfg));

  uint8_t val = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_icu_read_irqcr(20U, &val));
  /* Expected: IRQMD=10 (both), FCLKSEL=11 (PCLKB/64) at bit 4, FLTEN=1 at bit 7. */
  TEST_ASSERT_EQ(((2U << 0) | (3U << 4) | (1U << 7)), val);
  TEST_END("ra_icu_configure_irq_pin: IRQCRb channel 20");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_irq_pin(void)
{
  TEST_BEGIN("ra_icu_configure_irq_pin: rising-edge w/ filter");
  ra_sim_mmap_reset();
  (void)ra_icu_init();

  const ra_icu_irq_cfg_t cfg = {
    .sense      = k_ra_icu_irqmd_rising,
    .filter_div = k_ra_icu_fclksel_pclkb_32,
    .filter_en  = true,
  };
  TEST_ASSERT_EQ(k_ra_ok, ra_icu_configure_irq_pin(3U, &cfg));

  uint8_t val = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_icu_read_irqcr(3U, &val));
  /* Expected: IRQMD=01 (rising), FCLKSEL=10 (PCLKB/32) at bit 4, FLTEN=1 at bit 7. */
  TEST_ASSERT_EQ(((1U << 0) | (2U << 4) | (1U << 7)), val);
  TEST_END("ra_icu_configure_irq_pin: rising-edge w/ filter");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_irq_pin_bad_inputs(void)
{
  TEST_BEGIN("ra_icu_configure_irq_pin: bad inputs rejected");
  ra_sim_mmap_reset();
  (void)ra_icu_init();

  const ra_icu_irq_cfg_t cfg = {
    .sense      = k_ra_icu_irqmd_falling,
    .filter_div = k_ra_icu_fclksel_pclkb,
    .filter_en  = false,
  };
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_icu_configure_irq_pin(0U, nullptr));
  /* 33 is past the 32-channel RA8D2 limit (FSP BSP_FEATURE_ICU_IRQ_CHANNELS_MASK == 0xFFFFFFFF). */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_icu_configure_irq_pin(33U, &cfg));

  uint8_t val = 0U;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_icu_read_irqcr(0U, nullptr));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_icu_read_irqcr(33U, &val));
  TEST_END("ra_icu_configure_irq_pin: bad inputs rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_nmi_enable_disable_clear(void)
{
  TEST_BEGIN("ra_icu_nmi_enable / disable / clear");
  ra_sim_mmap_reset();
  (void)ra_icu_init();

  /* FSP R_ICU_NMIER uses bits 0..20. Test with a broad mask so the
   * 32-bit contract is exercised end-to-end. */
  TEST_ASSERT_EQ(k_ra_ok, ra_icu_nmi_enable(0x00105U));
  TEST_ASSERT_EQ(0x00105UL, *ra_icu_nmier());

  TEST_ASSERT_EQ(k_ra_ok, ra_icu_nmi_enable(0x10010U));
  TEST_ASSERT_EQ(0x10115UL, *ra_icu_nmier());

  TEST_ASSERT_EQ(k_ra_ok, ra_icu_nmi_disable(0x00004U));
  TEST_ASSERT_EQ(0x10111UL, *ra_icu_nmier());

  /* Status register reads + clear. */
  *ra_icu_nmisr() = 0x1F007AUL;
  uint32_t status = 0UL;
  TEST_ASSERT_EQ(k_ra_ok, ra_icu_nmi_status(&status));
  TEST_ASSERT_EQ(0x1F007AUL, status);

  TEST_ASSERT_EQ(k_ra_ok, ra_icu_nmi_clear(0x1FFFFFUL));
  TEST_ASSERT_EQ(0x1FFFFFUL, *ra_icu_nmiclr());

  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_icu_nmi_status(nullptr));
  TEST_END("ra_icu_nmi_enable / disable / clear");
}

int32_t main(void)
{
  test_init_clears_irqcr_and_nmi();
  test_configure_irq_pin();
  test_configure_irq_pin_bad_inputs();
  test_configure_irq_pin_irqcrb();
  test_nmi_enable_disable_clear();
  test_ielsr_mask_is_10_bits();
  (void)fprintf(stderr, "[OK  ] test_ra_icu.c\n");
  return 0;
}
