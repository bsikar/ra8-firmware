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

static void test_init_clears_irqcr_and_nmi(void)
{
  TEST_BEGIN("ra_icu_init: IRQCR + NMIER cleared");
  ra_sim_mmap_reset();

  /* Pre-pollute IRQCR0 and NMIER so we can observe the clear. */
  *ra_icu_irqcr(0U) = 0xFFU;
  *ra_icu_nmier()   = 0xFFU;

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_icu_init());
  TEST_ASSERT_EQ(0, (int)*ra_icu_irqcr(0U));
  TEST_ASSERT_EQ(0, (int)*ra_icu_nmier());
  TEST_END("ra_icu_init: IRQCR + NMIER cleared");
}

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
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_icu_configure_irq_pin(3U, &cfg));

  uint8_t val = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_icu_read_irqcr(3U, &val));
  /* Expected: IRQMD=01 (rising), FCLKSEL=10 (PCLKB/32) at bit 4, FLTEN=1 at bit 7. */
  TEST_ASSERT_EQ((int)((1U << 0) | (2U << 4) | (1U << 7)), (int)val);
  TEST_END("ra_icu_configure_irq_pin: rising-edge w/ filter");
}

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
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_icu_configure_irq_pin(0U, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_icu_configure_irq_pin(20U, &cfg));

  uint8_t val = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_icu_read_irqcr(0U, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_icu_read_irqcr(20U, &val));
  TEST_END("ra_icu_configure_irq_pin: bad inputs rejected");
}

static void test_nmi_enable_disable_clear(void)
{
  TEST_BEGIN("ra_icu_nmi_enable / disable / clear");
  ra_sim_mmap_reset();
  (void)ra_icu_init();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_icu_nmi_enable(0x05U));
  TEST_ASSERT_EQ((int)0x05, (int)*ra_icu_nmier());

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_icu_nmi_enable(0x10U));
  TEST_ASSERT_EQ((int)0x15, (int)*ra_icu_nmier());

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_icu_nmi_disable(0x04U));
  TEST_ASSERT_EQ((int)0x11, (int)*ra_icu_nmier());

  /* Status register reads + clear. */
  *ra_icu_nmisr() = 0x7AU;
  uint8_t status  = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_icu_nmi_status(&status));
  TEST_ASSERT_EQ((int)0x7A, (int)status);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_icu_nmi_clear(0xFFU));
  TEST_ASSERT_EQ((int)0xFF, (int)*ra_icu_nmiclr());

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_icu_nmi_status(nullptr));
  TEST_END("ra_icu_nmi_enable / disable / clear");
}

typedef enum : uint16_t {
  k_ra_icu_test_nvic_first = 0U,
  k_ra_icu_test_nvic_mid   = 37U,
  k_ra_icu_test_nvic_last  = 111U,
  k_ra_icu_test_nvic_bad   = 112U,
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

static void test_route_happy(void)
{
  TEST_BEGIN("icu route happy");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_icu_route((uint16_t)k_ra_icu_test_nvic_first, k_ra_elc_event_icu_irq0));
  volatile uint32_t* reg = ra_icu_ielsr((uint16_t)k_ra_icu_test_nvic_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ((int)k_ra_elc_event_icu_irq0, (int)*reg);
  TEST_END("icu route happy");
}

static void test_route_last_slot(void)
{
  TEST_BEGIN("icu route last slot");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_icu_route((uint16_t)k_ra_icu_test_nvic_last, k_ra_elc_event_icu_irq1));
  TEST_END("icu route last slot");
}

static void test_route_bad_slot(void)
{
  TEST_BEGIN("icu route bad slot");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_out_of_range,
                 (int)ra_icu_route((uint16_t)k_ra_icu_test_nvic_bad, k_ra_elc_event_none));
  TEST_END("icu route bad slot");
}

static void test_route_huge_slot(void)
{
  TEST_BEGIN("icu route huge slot");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_out_of_range,
                 (int)ra_icu_route((uint16_t)k_ra_icu_test_nvic_huge, k_ra_elc_event_none));
  TEST_END("icu route huge slot");
}

static void test_nvic_enable_first(void)
{
  TEST_BEGIN("icu nvic_enable first");
  ra_sim_mmap_reset();

  ra_icu_nvic_enable((uint16_t)k_ra_icu_test_nvic_first);
  const uint32_t word = *test_iser_word((uint16_t)k_ra_icu_test_nvic_first);
  TEST_ASSERT_EQ(1, (int)word);
  TEST_END("icu nvic_enable first");
}

static void test_nvic_enable_mid(void)
{
  TEST_BEGIN("icu nvic_enable mid");
  ra_sim_mmap_reset();

  ra_icu_nvic_enable((uint16_t)k_ra_icu_test_nvic_mid);
  /* nvic 37 -> word 1, bit 5. */
  const uint32_t expected = (uint32_t)(1UL << 5U);
  const uint32_t word     = *test_iser_word((uint16_t)k_ra_icu_test_nvic_mid);
  TEST_ASSERT_EQ((int)expected, (int)word);
  TEST_END("icu nvic_enable mid");
}

static void test_nvic_disable(void)
{
  TEST_BEGIN("icu nvic_disable");
  ra_sim_mmap_reset();

  ra_icu_nvic_disable((uint16_t)k_ra_icu_test_nvic_mid);
  const uint32_t expected = (uint32_t)(1UL << 5U);
  const uint32_t word     = *test_icer_word((uint16_t)k_ra_icu_test_nvic_mid);
  TEST_ASSERT_EQ((int)expected, (int)word);
  TEST_END("icu nvic_disable");
}

static void test_nvic_set_priority(void)
{
  TEST_BEGIN("icu nvic_set_priority");
  ra_sim_mmap_reset();

  ra_icu_nvic_set_priority((uint16_t)k_ra_icu_test_nvic_mid, (uint8_t)k_ra_icu_test_prio_half);
  const uint8_t expected =
    (uint8_t)((uint8_t)k_ra_icu_test_prio_half << (uint8_t)k_ra_icu_test_prio_shift);
  TEST_ASSERT_EQ((int)expected, (int)*test_ipr_byte((uint16_t)k_ra_icu_test_nvic_mid));

  /* Edge-case: priority zero and max. */
  ra_icu_nvic_set_priority((uint16_t)k_ra_icu_test_nvic_first, (uint8_t)k_ra_icu_test_prio_zero);
  TEST_ASSERT_EQ(0, (int)*test_ipr_byte((uint16_t)k_ra_icu_test_nvic_first));

  ra_icu_nvic_set_priority((uint16_t)k_ra_icu_test_nvic_last, (uint8_t)k_ra_icu_test_prio_max);
  const uint8_t expected_max =
    (uint8_t)((uint8_t)k_ra_icu_test_prio_max << (uint8_t)k_ra_icu_test_prio_shift);
  TEST_ASSERT_EQ((int)expected_max, (int)*test_ipr_byte((uint16_t)k_ra_icu_test_nvic_last));
  TEST_END("icu nvic_set_priority");
}

int main(void)
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
  test_nmi_enable_disable_clear();
  (void)fprintf(stderr, "[OK  ] test_ra_icu.c\n");
  return 0;
}
