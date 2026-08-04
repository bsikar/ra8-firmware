/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_icu_extint_demo.c
 * @brief Integration test: ICU external-interrupt-on-button demo
 *
 * @details
 * Mirrors examples/ek_ra8d2/icu_extint_demo/main.c bring-up:
 * ``ra8_icu_init`` -> ``ra8_icu_configure_irq_pin(13, falling_edge +
 * filter)`` -> ``ra8_icu_read_irqcr``. Backed by the host MMIO shim so
 * the IRQCR write/read round-trips succeed.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_icu.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_icu_extint_irq     = 13U, /**< SW1 -> IRQ13-DS.      */
  k_test_icu_extint_irq_oor = 99U, /**< Out of range (>= 32). */
} test_icu_extint_irq_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

static ra8_icu_irq_cfg_t make_cfg(void)
{
  const ra8_icu_irq_cfg_t cfg = {
    .sense      = k_ra8_icu_irqmd_falling,
    .filter_div = k_ra8_icu_fclksel_pclkb_64,
    .filter_en  = true,
  };
  return cfg;
}

/**
 * @brief Golden bring-up: init + configure IRQ13 + readback IRQCR.
 *
 * @par MC/DC:
 * Decision in app: ``ra8_icu_configure_irq_pin != ok``. One atomic
 * condition x 2 vectors -- golden (this) + null cfg / bad channel
 * below.
 */
static void test_icu_extint_arm_ok(void)
{
  reset_world();
  TEST_BEGIN("icu_extint_demo: configure IRQ13 + readback");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_icu_init());
  const ra8_icu_irq_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_icu_configure_irq_pin((uint8_t)k_test_icu_extint_irq, &cfg));
  uint8_t irqcr = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_icu_read_irqcr((uint8_t)k_test_icu_extint_irq, &irqcr));
  TEST_END("icu_extint_demo: configure IRQ13 + readback");
}

/**
 * @brief NULL cfg rejected by ``ra8_icu_configure_irq_pin``.
 *
 * @par MC/DC:
 * Decision: ``cfg == NULL``. One atomic condition x 2 vectors --
 * non-NULL above + NULL here.
 */
static void test_icu_extint_null_cfg(void)
{
  reset_world();
  TEST_BEGIN("icu_extint_demo: NULL cfg rejected");
  (void)ra8_icu_init();
  TEST_ASSERT(ra8_icu_configure_irq_pin((uint8_t)k_test_icu_extint_irq, nullptr) != k_ra8_ok);
  TEST_END("icu_extint_demo: NULL cfg rejected");
}

/**
 * @brief Out-of-range IRQ number rejected.
 *
 * @par MC/DC:
 * Decision: ``irq_num < k_ra8_icu_max_irq``. One atomic condition x 2
 * vectors -- in-range golden + out-of-range here.
 */
static void test_icu_extint_bad_irq(void)
{
  reset_world();
  TEST_BEGIN("icu_extint_demo: out-of-range IRQ rejected");
  (void)ra8_icu_init();
  const ra8_icu_irq_cfg_t cfg = make_cfg();
  TEST_ASSERT(ra8_icu_configure_irq_pin((uint8_t)k_test_icu_extint_irq_oor, &cfg) != k_ra8_ok);
  TEST_END("icu_extint_demo: out-of-range IRQ rejected");
}

/**
 * @brief NULL ``out_val`` rejected by ``ra8_icu_read_irqcr``.
 *
 * @par MC/DC:
 * Decision: ``out_val == NULL``. One atomic condition x 2 vectors --
 * non-NULL golden + NULL here.
 */
static void test_icu_extint_read_null(void)
{
  reset_world();
  TEST_BEGIN("icu_extint_demo: NULL out_val rejected");
  (void)ra8_icu_init();
  TEST_ASSERT(ra8_icu_read_irqcr((uint8_t)k_test_icu_extint_irq, nullptr) != k_ra8_ok);
  TEST_END("icu_extint_demo: NULL out_val rejected");
}

int main(void)
{
  test_icu_extint_arm_ok();
  test_icu_extint_null_cfg();
  test_icu_extint_bad_irq();
  test_icu_extint_read_null();
  return 0;
}
