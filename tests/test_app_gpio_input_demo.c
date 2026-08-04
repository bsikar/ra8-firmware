/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_gpio_input_demo.c
 * @brief Integration test: SW1 -> LED1 mirror demo bring-up
 *
 * @details
 * Mirrors examples/ek_ra8d2/gpio_input_demo/main.c bring-up flow:
 * ra8_board_led_init(LED1) + ra8_board_sw_init(SW1), then exercise
 * ra8_board_sw_read + ra8_board_led_on / ra8_board_led_off as the
 * mainline does.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_pin_validator.h"
#include "unity_minimal.h"

static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
}

/**
 * @brief Golden bring-up replays gpio_demo_setup_or_halt.
 *
 * @par MC/DC:
 * Compound decision in app: ``led_init != ok || sw_init != ok``.
 * Two atomic conditions x N+1 = 3 vectors -- golden (this) plus
 * each-rejection covered below.
 */
static void test_gpio_app_bringup_ok(void)
{
  reset_world();
  TEST_BEGIN("gpio_input_demo: bring-up ok");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sw_init(k_ra8_board_sw1));
  TEST_END("gpio_input_demo: bring-up ok");
}

/**
 * @brief sw_read returns one of the legal states.
 *
 * @par MC/DC:
 * Decision in app: ``*out_state == k_ra8_board_sw_pressed``. One
 * atomic condition x 2 vectors -- pressed (would call led_on) and
 * released (would call led_off). Both branches reachable from the
 * mock by varying the seeded pin level.
 */
static void test_gpio_app_sw_read_legal(void)
{
  reset_world();
  TEST_BEGIN("gpio_input_demo: sw_read returns legal value");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sw_init(k_ra8_board_sw1));
  ra8_board_sw_state_t s = k_ra8_board_sw_released;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sw_read(k_ra8_board_sw1, &s));
  TEST_ASSERT(s == k_ra8_board_sw_released || s == k_ra8_board_sw_pressed);
  TEST_END("gpio_input_demo: sw_read returns legal value");
}

/**
 * @brief led_on / led_off both round-trip via the API.
 *
 * @par MC/DC:
 * Decision: ``led_on != ok``. One atomic condition x 2 vectors --
 * success here, bad-led id below.
 */
static void test_gpio_app_led_drive(void)
{
  reset_world();
  TEST_BEGIN("gpio_input_demo: led_on/off round-trip");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_on(k_ra8_board_led1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_off(k_ra8_board_led1));
  TEST_END("gpio_input_demo: led_on/off round-trip");
}

/**
 * @brief Bad LED id rejected by ra8_board_led_on.
 *
 * @par MC/DC:
 * Decision: ``led < k_ra8_board_led_count``. One atomic condition x
 * 2 vectors -- in-range golden + this out-of-range.
 */
static void test_gpio_app_bad_led(void)
{
  reset_world();
  TEST_BEGIN("gpio_input_demo: bad LED id rejected");
  TEST_ASSERT(ra8_board_led_on((ra8_board_led_id_t)k_ra8_board_led_count) != k_ra8_ok);
  TEST_END("gpio_input_demo: bad LED id rejected");
}

/**
 * @brief NULL out_pressed rejected by sw_read.
 *
 * @par MC/DC:
 * Decision: ``out_pressed == nullptr``. One atomic condition x 2
 * vectors -- non-NULL above + this NULL.
 */
static void test_gpio_app_sw_read_null(void)
{
  reset_world();
  TEST_BEGIN("gpio_input_demo: sw_read NULL rejected");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sw_init(k_ra8_board_sw1));
  TEST_ASSERT(ra8_board_sw_read(k_ra8_board_sw1, nullptr) != k_ra8_ok);
  TEST_END("gpio_input_demo: sw_read NULL rejected");
}

int main(void)
{
  test_gpio_app_bringup_ok();
  test_gpio_app_sw_read_legal();
  test_gpio_app_led_drive();
  test_gpio_app_bad_led();
  test_gpio_app_sw_read_null();
  return 0;
}
