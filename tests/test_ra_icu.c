/**
 * @file test_ra_icu.c
 * @brief Unit tests for ra_icu.c (Interrupt Control Unit)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_elc_regs.h"
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

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_icu_init());
  TEST_ASSERT_EQ(0, (int32_t)*ra_icu_irqcr(0U));
  TEST_ASSERT_EQ(0, (int32_t)*ra_icu_irqcr(16U));
  TEST_ASSERT_EQ((int64_t)0UL, (int64_t)*ra_icu_nmier());
  TEST_ASSERT_EQ((int64_t)0UL, (int64_t)*ra_icu_wupen0());
  TEST_ASSERT_EQ((int64_t)0UL, (int64_t)*ra_icu_wupen1());
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
  TEST_ASSERT_EQ((int32_t)0x3FF, (int32_t)k_ra_ielsr_iels_mask);
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
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_icu_configure_irq_pin(20U, &cfg));

  uint8_t val = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_icu_read_irqcr(20U, &val));
  /* Expected: IRQMD=10 (both), FCLKSEL=11 (PCLKB/64) at bit 4, FLTEN=1 at bit 7. */
  TEST_ASSERT_EQ((int32_t)((2U << 0) | (3U << 4) | (1U << 7)), (int32_t)val);
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
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_icu_configure_irq_pin(3U, &cfg));

  uint8_t val = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_icu_read_irqcr(3U, &val));
  /* Expected: IRQMD=01 (rising), FCLKSEL=10 (PCLKB/32) at bit 4, FLTEN=1 at bit 7. */
  TEST_ASSERT_EQ((int32_t)((1U << 0) | (2U << 4) | (1U << 7)), (int32_t)val);
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
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_icu_configure_irq_pin(0U, nullptr));
  /* 33 is past the 32-channel RA8D2 limit (FSP BSP_FEATURE_ICU_IRQ_CHANNELS_MASK == 0xFFFFFFFF). */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_icu_configure_irq_pin(33U, &cfg));

  uint8_t val = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_icu_read_irqcr(0U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_icu_read_irqcr(33U, &val));
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
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_icu_nmi_enable(0x00105U));
  TEST_ASSERT_EQ((int64_t)0x00105UL, (int64_t)*ra_icu_nmier());

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_icu_nmi_enable(0x10010U));
  TEST_ASSERT_EQ((int64_t)0x10115UL, (int64_t)*ra_icu_nmier());

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_icu_nmi_disable(0x00004U));
  TEST_ASSERT_EQ((int64_t)0x10111UL, (int64_t)*ra_icu_nmier());

  /* Status register reads + clear. */
  *ra_icu_nmisr() = 0x1F007AUL;
  uint32_t status = 0UL;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_icu_nmi_status(&status));
  TEST_ASSERT_EQ((int64_t)0x1F007AUL, (int64_t)status);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_icu_nmi_clear(0x1FFFFFUL));
  TEST_ASSERT_EQ((int64_t)0x1FFFFFUL, (int64_t)*ra_icu_nmiclr());

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_icu_nmi_status(nullptr));
  TEST_END("ra_icu_nmi_enable / disable / clear");
}

typedef enum : uint16_t {
  k_ra_icu_test_nvic_first = 0U,
  k_ra_icu_test_nvic_mid   = 37U,
  k_ra_icu_test_nvic_last  = 95U, /**< FSP R_ICU IELSR[96] -> last index 95. */
  k_ra_icu_test_nvic_bad   = 96U,
  k_ra_icu_test_nvic_huge  = 800U,
} ra_icu_test_nvic_t;

typedef enum : uint8_t {
  k_ra_icu_test_prio_zero  = 0U,
  k_ra_icu_test_prio_half  = 8U,
  k_ra_icu_test_prio_max   = 15U,
  k_ra_icu_test_prio_shift = 4U,
} ra_icu_test_prio_t;

typedef enum : uintptr_t {
  k_ra_icu_test_nvic_iser_base = 0xE000E100UL,
  k_ra_icu_test_nvic_icer_base = 0xE000E180UL,
  k_ra_icu_test_nvic_ipr_base  = 0xE000E400UL,
} ra_icu_test_nvic_addr_t;

typedef enum : uint32_t {
  k_ra_icu_test_bits_per_word = 32U,
} ra_icu_test_layout_t;

static volatile uint32_t* test_iser_word(uint16_t nvic)
{
  const uint16_t word = nvic / (uint16_t)k_ra_icu_test_bits_per_word;
  return (volatile uint32_t*)(k_ra_icu_test_nvic_iser_base + ((uintptr_t)word * sizeof(uint32_t)));
}

static volatile uint32_t* test_icer_word(uint16_t nvic)
{
  const uint16_t word = nvic / (uint16_t)k_ra_icu_test_bits_per_word;
  return (volatile uint32_t*)(k_ra_icu_test_nvic_icer_base + ((uintptr_t)word * sizeof(uint32_t)));
}

static volatile uint8_t* test_ipr_byte(uint16_t nvic)
{
  return (volatile uint8_t*)(k_ra_icu_test_nvic_ipr_base + (uintptr_t)nvic);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_route_happy(void)
{
  TEST_BEGIN("icu route happy");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_icu_route((uint16_t)k_ra_icu_test_nvic_first, k_ra_elc_event_icu_irq0));
  volatile uint32_t* reg = ra_icu_ielsr((uint16_t)k_ra_icu_test_nvic_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ((int32_t)k_ra_elc_event_icu_irq0, (int32_t)*reg);
  TEST_END("icu route happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_route_last_slot(void)
{
  TEST_BEGIN("icu route last slot");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_icu_route((uint16_t)k_ra_icu_test_nvic_last, k_ra_elc_event_icu_irq1));
  TEST_END("icu route last slot");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_route_bad_slot(void)
{
  TEST_BEGIN("icu route bad slot");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_icu_route((uint16_t)k_ra_icu_test_nvic_bad, k_ra_elc_event_none));
  TEST_END("icu route bad slot");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_route_huge_slot(void)
{
  TEST_BEGIN("icu route huge slot");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_icu_route((uint16_t)k_ra_icu_test_nvic_huge, k_ra_elc_event_none));
  TEST_END("icu route huge slot");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_nvic_enable_first(void)
{
  TEST_BEGIN("icu nvic_enable first");
  ra_sim_mmap_reset();

  ra_icu_nvic_enable((uint16_t)k_ra_icu_test_nvic_first);
  const uint32_t word = *test_iser_word((uint16_t)k_ra_icu_test_nvic_first);
  TEST_ASSERT_EQ(1, (int32_t)word);
  TEST_END("icu nvic_enable first");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_nvic_enable_mid(void)
{
  TEST_BEGIN("icu nvic_enable mid");
  ra_sim_mmap_reset();

  ra_icu_nvic_enable((uint16_t)k_ra_icu_test_nvic_mid);
  /* nvic 37 -> word 1, bit 5. */
  const uint32_t expected = (uint32_t)(1UL << 5U);
  const uint32_t word     = *test_iser_word((uint16_t)k_ra_icu_test_nvic_mid);
  TEST_ASSERT_EQ((int32_t)expected, (int32_t)word);
  TEST_END("icu nvic_enable mid");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_nvic_disable(void)
{
  TEST_BEGIN("icu nvic_disable");
  ra_sim_mmap_reset();

  ra_icu_nvic_disable((uint16_t)k_ra_icu_test_nvic_mid);
  const uint32_t expected = (uint32_t)(1UL << 5U);
  const uint32_t word     = *test_icer_word((uint16_t)k_ra_icu_test_nvic_mid);
  TEST_ASSERT_EQ((int32_t)expected, (int32_t)word);
  TEST_END("icu nvic_disable");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_nvic_set_priority(void)
{
  TEST_BEGIN("icu nvic_set_priority");
  ra_sim_mmap_reset();

  ra_icu_nvic_set_priority((uint16_t)k_ra_icu_test_nvic_mid, (uint8_t)k_ra_icu_test_prio_half);
  const uint8_t expected =
    (uint8_t)((uint8_t)k_ra_icu_test_prio_half << (uint8_t)k_ra_icu_test_prio_shift);
  TEST_ASSERT_EQ((int32_t)expected, (int32_t)*test_ipr_byte((uint16_t)k_ra_icu_test_nvic_mid));

  /* Edge-case: priority zero and max. */
  ra_icu_nvic_set_priority((uint16_t)k_ra_icu_test_nvic_first, (uint8_t)k_ra_icu_test_prio_zero);
  TEST_ASSERT_EQ(0, (int32_t)*test_ipr_byte((uint16_t)k_ra_icu_test_nvic_first));

  ra_icu_nvic_set_priority((uint16_t)k_ra_icu_test_nvic_last, (uint8_t)k_ra_icu_test_prio_max);
  const uint8_t expected_max =
    (uint8_t)((uint8_t)k_ra_icu_test_prio_max << (uint8_t)k_ra_icu_test_prio_shift);
  TEST_ASSERT_EQ((int32_t)expected_max, (int32_t)*test_ipr_byte((uint16_t)k_ra_icu_test_nvic_last));
  TEST_END("icu nvic_set_priority");
}

int32_t main(void)
{
  test_route_happy();
  test_route_last_slot();
  test_route_bad_slot();
  test_route_huge_slot();
  test_nvic_enable_first();
  test_nvic_enable_mid();
  test_nvic_disable();
  test_nvic_set_priority();
  test_init_clears_irqcr_and_nmi();
  test_configure_irq_pin();
  test_configure_irq_pin_bad_inputs();
  test_configure_irq_pin_irqcrb();
  test_nmi_enable_disable_clear();
  test_ielsr_mask_is_10_bits();
  (void)fprintf(stderr, "[OK  ] test_ra_icu.c\n");
  return 0;
}
