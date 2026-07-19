/**
 * @file test_ra8_icu.c
 * @brief Unit tests for ra8_icu.c (Interrupt Control Unit)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_icu.h"
#include "ra8_icu_regs.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum icu_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_icu_irqcr_all_bits =
    0xFFU, /**< Every IRQCR bit set, so a write that reached the wrong channel leaves evidence. */
} icu_uint8_const_t;

/**
 * @enum icu_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_icu_probe_wupen0 =
    0xCAFEBABEUL, /**< Planted in WUPEN0 to prove the write reaches the register. */
  k_icu_probe_wupen1 =
    0xDEADBEEFUL, /**< Planted in WUPEN1; different from the WUPEN0 value so the two cannot be confused. */
  k_icu_nmisr_mixed =
    0x1F007AUL, /**< An NMISR value with a scattered mix of set and clear bits, so a mask applied to the wrong field changes the result. */
  k_icu_nmier_all_sources =
    0x1FFFFFUL, /**< Every implemented NMIER enable bit, the widest legal value for the register. */
} icu_uint32_const_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_clears_irqcr_and_nmi(void)
{
  TEST_BEGIN("ra8_icu_init: IRQCR + NMIER + WUPEN cleared");
  ra8_sim_mmap_reset();

  /* Pre-pollute IRQCR0, IRQCR16 (IRQCRb[0]), NMIER, WUPEN0, WUPEN1 so
   * we can observe the clear. */
  *ra8_icu_irqcr(0U)  = k_icu_irqcr_all_bits;
  *ra8_icu_irqcr(16U) = k_icu_irqcr_all_bits;
  *ra8_icu_nmier()    = k_icu_nmier_all_sources;
  *ra8_icu_wupen0()   = k_icu_probe_wupen0;
  *ra8_icu_wupen1()   = k_icu_probe_wupen1;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_icu_init());
  TEST_ASSERT_EQ(0, *ra8_icu_irqcr(0U));
  TEST_ASSERT_EQ(0, *ra8_icu_irqcr(16U));
  TEST_ASSERT_EQ(0UL, *ra8_icu_nmier());
  TEST_ASSERT_EQ(0UL, *ra8_icu_wupen0());
  TEST_ASSERT_EQ(0UL, *ra8_icu_wupen1());
  TEST_END("ra8_icu_init: IRQCR + NMIER + WUPEN cleared");
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
  TEST_ASSERT_EQ(0x3FF, k_ra8_ielsr_iels_mask);
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
  TEST_BEGIN("ra8_icu_configure_irq_pin: IRQCRb channel 20");
  ra8_sim_mmap_reset();
  (void)ra8_icu_init();

  const ra8_icu_irq_cfg_t cfg = {
    .sense      = k_ra8_icu_irqmd_both,
    .filter_div = k_ra8_icu_fclksel_pclkb_64,
    .filter_en  = true,
  };
  /* Channel 20 (IRQCRb[4]) lives at base + 0x14 + 4 = base + 0x18. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_icu_configure_irq_pin(20U, &cfg));

  uint8_t val = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_icu_read_irqcr(20U, &val));
  /* Expected: IRQMD=10 (both), FCLKSEL=11 (PCLKB/64) at bit 4, FLTEN=1 at bit 7. */
  TEST_ASSERT_EQ(((2U << 0) | (3U << 4) | (1U << 7)), val);
  TEST_END("ra8_icu_configure_irq_pin: IRQCRb channel 20");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_irq_pin(void)
{
  TEST_BEGIN("ra8_icu_configure_irq_pin: rising-edge w/ filter");
  ra8_sim_mmap_reset();
  (void)ra8_icu_init();

  const ra8_icu_irq_cfg_t cfg = {
    .sense      = k_ra8_icu_irqmd_rising,
    .filter_div = k_ra8_icu_fclksel_pclkb_32,
    .filter_en  = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_icu_configure_irq_pin(3U, &cfg));

  uint8_t val = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_icu_read_irqcr(3U, &val));
  /* Expected: IRQMD=01 (rising), FCLKSEL=10 (PCLKB/32) at bit 4, FLTEN=1 at bit 7. */
  TEST_ASSERT_EQ(((1U << 0) | (2U << 4) | (1U << 7)), val);
  TEST_END("ra8_icu_configure_irq_pin: rising-edge w/ filter");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_irq_pin_bad_inputs(void)
{
  TEST_BEGIN("ra8_icu_configure_irq_pin: bad inputs rejected");
  ra8_sim_mmap_reset();
  (void)ra8_icu_init();

  const ra8_icu_irq_cfg_t cfg = {
    .sense      = k_ra8_icu_irqmd_falling,
    .filter_div = k_ra8_icu_fclksel_pclkb,
    .filter_en  = false,
  };
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_icu_configure_irq_pin(0U, nullptr));
  /* 33 is past the 32-channel RA8D2 limit (FSP BSP_FEATURE_ICU_IRQ_CHANNELS_MASK == 0xFFFFFFFF). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_icu_configure_irq_pin(33U, &cfg));

  uint8_t val = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_icu_read_irqcr(0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_icu_read_irqcr(33U, &val));
  TEST_END("ra8_icu_configure_irq_pin: bad inputs rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_nmi_enable_disable_clear(void)
{
  TEST_BEGIN("ra8_icu_nmi_enable / disable / clear");
  ra8_sim_mmap_reset();
  (void)ra8_icu_init();

  /* FSP R_ICU_NMIER uses bits 0..20. Test with a broad mask so the
   * 32-bit contract is exercised end-to-end. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_icu_nmi_enable(0x00105U));
  TEST_ASSERT_EQ(0x00105UL, *ra8_icu_nmier());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_icu_nmi_enable(0x10010U));
  TEST_ASSERT_EQ(0x10115UL, *ra8_icu_nmier());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_icu_nmi_disable(0x00004U));
  TEST_ASSERT_EQ(0x10111UL, *ra8_icu_nmier());

  /* Status register reads + clear. */
  *ra8_icu_nmisr() = k_icu_nmisr_mixed;
  uint32_t status  = 0UL;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_icu_nmi_status(&status));
  TEST_ASSERT_EQ(0x1F007AUL, status);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_icu_nmi_clear(0x1FFFFFUL));
  TEST_ASSERT_EQ(0x1FFFFFUL, *ra8_icu_nmiclr());

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_icu_nmi_status(nullptr));
  TEST_END("ra8_icu_nmi_enable / disable / clear");
}

int32_t main(void)
{
  test_init_clears_irqcr_and_nmi();
  test_configure_irq_pin();
  test_configure_irq_pin_bad_inputs();
  test_configure_irq_pin_irqcrb();
  test_nmi_enable_disable_clear();
  test_ielsr_mask_is_10_bits();
  (void)fprintf(stderr, "[OK  ] test_ra8_icu.c\n");
  return 0;
}
