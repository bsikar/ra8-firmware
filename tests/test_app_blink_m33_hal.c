/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Brighton Sikarskie */
/**
 * @file test_app_blink_m33_hal.c
 * @brief App-level test: the blink_m33_hal example's CPU1 blink step drives LED1
 *
 * @details
 * Exercises the actual blink logic of the `blink_m33_hal` example (issue #580) --
 * not a re-implementation. The example's CPU1 loop (`cpu1_main.c`) advances the
 * blink by calling `blink_m33_hal_step()` from the app's own header
 * (`examples/.../blink_m33_hal/blink_m33_hal.h`); this test drives that SAME
 * function against the `ra8_fake_mmap` peripheral-RAM backing and asserts the
 * PORT6 (LED1 / P600) PCNTR1 effect: the pin is held an output and its level
 * follows the toggled state, siblings are preserved, and a bounded run of the
 * loop produces the alternating blink. Because it links the app's header, the
 * tested step IS the code the firmware runs.
 *
 * The step has no compound boolean decision (its only branch is the single
 * `level == k_ra8_level_high` toggle select), so there are no MC/DC vectors to
 * enumerate; the low/high cases still drive both branches for full coverage.
 */

#include "blink_m33_hal.h"
#include "ra8_fake_mmap.h"
#include "ra8_port_constants.h"
#include "ra8_port_regs.h"
#include "unity_minimal.h"

/**
 * @enum app_blink_m33_hal_ids_t
 * @brief Fixed test constants for the blink_m33_hal app test.
 */
typedef enum : uint8_t {
  k_blink_sibling_pin = 3U, /**< A sibling pin on the LED port the RMW must preserve. */
  k_blink_iters       = 4U, /**< Bounded blink-loop iterations to walk.               */
} app_blink_m33_hal_ids_t;

/**
 * @brief Reset the mock MMIO backing before each case.
 */
static void reset_state(void)
{
  ra8_fake_mmap_reset();
}

/**
 * @brief Low-half (PDR) mask for a pin.
 * @param[in] pin Pin index 0..15.
 * @return `1U << pin`.
 */
static uint32_t pdr_mask(uint32_t pin)
{
  return (uint32_t)(1UL << pin);
}

/**
 * @brief High-half (PODR) mask for a pin.
 * @param[in] pin Pin index 0..15.
 * @return `1U << (16 + pin)`.
 */
static uint32_t podr_mask(uint32_t pin)
{
  return (uint32_t)(1UL << pin) << (uint32_t)k_ra8_pcntr_high_half_shift;
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- drives the toggle from LOW,
 * i.e. the `level != high` branch, so LED1 is driven HIGH)
 */
static void test_step_from_low_drives_led_high(void)
{
  TEST_BEGIN("blink_m33_hal: step from low drives LED1 high");
  reset_state();

  const ra8_level_t next = blink_m33_hal_step(k_ra8_level_low);
  TEST_ASSERT_EQ(k_ra8_level_high, next);

  const uint32_t          led_pin = (uint32_t)RA8_PIN_PIN(k_ra8_pin_led1);
  volatile r_port_regs_t* reg     = ra8_port(RA8_PIN_PORT(k_ra8_pin_led1));
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* HUM Ch 20.2.1 "PCNTR1/PODR/PDR : Port Control Register 1" p 840 */
  TEST_ASSERT_EQ(pdr_mask(led_pin) | podr_mask(led_pin), reg->PCNTR1);
  TEST_END("blink_m33_hal: step from low drives LED1 high");
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- drives the toggle from HIGH,
 * i.e. the `level == high` branch, so LED1 is driven LOW)
 */
static void test_step_from_high_drives_led_low(void)
{
  TEST_BEGIN("blink_m33_hal: step from high drives LED1 low");
  reset_state();

  const ra8_level_t next = blink_m33_hal_step(k_ra8_level_high);
  TEST_ASSERT_EQ(k_ra8_level_low, next);

  const uint32_t          led_pin = (uint32_t)RA8_PIN_PIN(k_ra8_pin_led1);
  volatile r_port_regs_t* reg     = ra8_port(RA8_PIN_PORT(k_ra8_pin_led1));
  /* Output direction set (PDR), output latch cleared (no PODR). */
  /* HUM Ch 20.2.1 "PCNTR1/PODR/PDR : Port Control Register 1" p 840 */
  TEST_ASSERT_EQ(pdr_mask(led_pin), reg->PCNTR1);
  TEST_END("blink_m33_hal: step from high drives LED1 low");
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- proves the step's PCNTR1
 * read-modify-write leaves an unrelated sibling pin on the LED port untouched)
 */
static void test_step_preserves_sibling(void)
{
  TEST_BEGIN("blink_m33_hal: step preserves a sibling pin");
  reset_state();

  volatile r_port_regs_t* reg = ra8_port(RA8_PIN_PORT(k_ra8_pin_led1));
  /* A sibling pin on the LED port pre-configured output + high. */
  /* HUM Ch 20.2.1 "PCNTR1/PODR/PDR : Port Control Register 1" p 840 */
  reg->PCNTR1 = pdr_mask((uint32_t)k_blink_sibling_pin) | podr_mask((uint32_t)k_blink_sibling_pin);

  (void)blink_m33_hal_step(k_ra8_level_low); /* drives LED1 high */

  const uint32_t led_pin  = (uint32_t)RA8_PIN_PIN(k_ra8_pin_led1);
  const uint32_t expected = pdr_mask((uint32_t)k_blink_sibling_pin) |
                            podr_mask((uint32_t)k_blink_sibling_pin) | pdr_mask(led_pin) |
                            podr_mask(led_pin);
  /* HUM Ch 20.2.1 "PCNTR1/PODR/PDR : Port Control Register 1" p 840 */
  TEST_ASSERT_EQ(expected, reg->PCNTR1);
  TEST_END("blink_m33_hal: step preserves a sibling pin");
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- walks a bounded run of the
 * blink loop and asserts each iteration's LED1 level, exercising both toggle
 * branches in sequence exactly as cpu1_main() does)
 */
static void test_blink_loop_alternates(void)
{
  TEST_BEGIN("blink_m33_hal: bounded blink loop alternates LED1");
  reset_state();

  const uint32_t          led_pin = (uint32_t)RA8_PIN_PIN(k_ra8_pin_led1);
  volatile r_port_regs_t* reg     = ra8_port(RA8_PIN_PORT(k_ra8_pin_led1));

  ra8_level_t level = k_ra8_level_low;
  for (uint32_t i = 0U; i < (uint32_t)k_blink_iters; i++) {
    level = blink_m33_hal_step(level);
    /* Starting from low, odd steps are high, even steps are low. */
    const bool expect_high = ((i % 2U) == 0U);
    TEST_ASSERT_EQ(expect_high ? k_ra8_level_high : k_ra8_level_low, level);

    /* PDR is always set (output); PODR mirrors the current level. */
    const uint32_t expected =
      expect_high ? (pdr_mask(led_pin) | podr_mask(led_pin)) : pdr_mask(led_pin);
    /* HUM Ch 20.2.1 "PCNTR1/PODR/PDR : Port Control Register 1" p 840 */
    TEST_ASSERT_EQ(expected, reg->PCNTR1);
  }
  TEST_END("blink_m33_hal: bounded blink loop alternates LED1");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 * @details main() walks this table, so adding a case is a one-line edit.
 * @note Order is significant: cases run top to bottom.
 */
static void (*const s_test_roster[])(void) = {
  test_step_from_low_drives_led_high,
  test_step_from_high_drives_led_low,
  test_step_preserves_sibling,
  test_blink_loop_alternates,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_app_blink_m33_hal.c\n");
  return 0;
}
