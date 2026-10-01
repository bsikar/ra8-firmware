/**
 * @file test_ra8_board_ek_ra8d2_gpt_profile.c
 * @brief Vectors for the EK-RA8D2 timer and PWM profiles.
 *
 * @par Tag
 * [Ring 5 / Test] {World: NS}
 *
 * @details
 * Driven through the board handles and the public facades, with GPT and PFS
 * state read back from the fake memory map. What is proven is the board's
 * numbering (which board index lands on which GPT channel and pin), that the
 * two sets are disjoint, and that the pin claim follows open and close. Not
 * proven: that the pins carry a waveform, which needs SW4-4 and a scope.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "fw_if_pwm.h"
#include "fw_if_timer.h"
#include "ra8_board_ek_ra8d2_connectors.h"
#include "ra8_board_ek_ra8d2_gpt_profile.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_gpio_constants.h"
#include "ra8_gpt.h"
#include "ra8_gpt_regs.h"
#include "ra8_pfs_regs.h"
#include "ra8_pin_validator.h"
#include "unity_minimal.h"

/** @brief Fixed inputs and expected register values. */
typedef enum : uint32_t {
  k_test_period   = 999U,         /**< Any valid period.              */
  k_test_ch1_bit  = 0x00000002UL, /**< CSTRT1: PWM 0 lives on GPT1.   */
  k_test_ch3_bit  = 0x00000008UL, /**< CSTRT3: timer 1 lives on GPT3. */
  k_test_ch1      = 1U,           /**< GPT1.                          */
  k_test_ch3      = 3U,           /**< GPT3.                          */
  k_test_pfs_mask = 0x1F010000UL, /**< PSEL[4:0] and PMR.             */
  k_test_pfs_gpt  = 0x03010000UL, /**< PSEL = GPT, PMR = peripheral.  */
} test_const_t;

/** @brief Clean fake registers and a free pin map. */
static void reset_state(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
}

/** @brief PmnPFS of Arduino D6 (P105). */
static uint32_t pfs_d6(void)
{
  const ra8_port_pin_t pin = (ra8_port_pin_t)k_ra8_board_arduino_d6;
  return *ra8_pfs_pmn(RA8_PIN_PORT(pin), RA8_PIN_PIN(pin));
}

static void test_lookups(void)
{
  TEST_BEGIN("lookups: PWM 1,2,8; timers 0,3,4,5,6,7,9; past the end not found");
  static const uint8_t k_pwm[]   = {1U, 2U, 8U};
  static const uint8_t k_timer[] = {0U, 3U, 4U, 5U, 6U, 7U, 9U};
  uint8_t              chip      = 0U;
  for (uint8_t i = 0U; i < k_ra8_board_pwm_count; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_board_pwm_to_chip(i, &chip));
    TEST_ASSERT_EQ(k_pwm[i], chip);
  }
  for (uint8_t i = 0U; i < k_ra8_board_timer_count; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_board_timer_to_chip(i, &chip));
    TEST_ASSERT_EQ(k_timer[i], chip);
  }
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_board_pwm_to_chip(k_ra8_board_pwm_count, &chip));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_board_timer_to_chip(k_ra8_board_timer_count, &chip));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_pwm_to_chip(0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_timer_to_chip(0U, nullptr));
  TEST_END("lookups: PWM 1,2,8; timers 0,3,4,5,6,7,9; past the end not found");
}

static void test_sets_are_disjoint(void)
{
  TEST_BEGIN("no GPT channel is both a board timer and a board PWM output");
  for (uint8_t t = 0U; t < k_ra8_board_timer_count; ++t) {
    uint8_t tc = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_board_timer_to_chip(t, &tc));
    for (uint8_t p = 0U; p < k_ra8_board_pwm_count; ++p) {
      uint8_t pc = 0U;
      TEST_ASSERT_EQ(k_ra8_ok, ra8_board_pwm_to_chip(p, &pc));
      TEST_ASSERT(tc != pc);
    }
  }
  TEST_END("no GPT channel is both a board timer and a board PWM output");
}

static void test_caps_carry_board_counts(void)
{
  TEST_BEGIN("board handles report board counts and the chip's limits");
  fw_timer_caps_t tc = {};
  fw_pwm_caps_t   pc = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_get_caps(ra8_board_timer(), &tc));
  TEST_ASSERT_EQ(k_ra8_board_timer_count, tc.channel_count);
  TEST_ASSERT_EQ(UINT32_MAX, tc.counter_max);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_get_caps(ra8_board_pwm(), &pc));
  TEST_ASSERT_EQ(k_ra8_board_pwm_count, pc.channel_count);
  TEST_ASSERT_EQ(UINT32_MAX - 1U, pc.period_max);
  TEST_END("board handles report board counts and the chip's limits");
}

static void test_timer_lands_on_its_channel(void)
{
  TEST_BEGIN("board timer 1 starts GPT3, and only GPT3");
  reset_state();
  const fw_timer_t*   tmr = ra8_board_timer();
  const fw_timer_ch_t t1  = {.index = 1U};
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_open(tmr, t1, k_fw_timer_mode_free_run, k_test_period));
  TEST_ASSERT_EQ(k_test_period, ra8_gpt((uint8_t)k_test_ch3)->GTPR);
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_start(tmr, t1));
  TEST_ASSERT_EQ(k_test_ch3_bit, ra8_gpt((uint8_t)k_test_ch3)->GTSTR);
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_close(tmr, t1));
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 fw_timer_open(tmr,
                               (fw_timer_ch_t){.index = k_ra8_board_timer_count},
                               k_fw_timer_mode_free_run,
                               k_test_period));
  TEST_END("board timer 1 starts GPT3, and only GPT3");
}

static void test_pwm_routes_and_releases_its_pin(void)
{
  TEST_BEGIN("board PWM 0 routes D6 to GPT1, claims it, releases on close");
  reset_state();
  const fw_pwm_t*      pwm = ra8_board_pwm();
  const fw_pwm_ch_t    p0  = {.index = k_ra8_board_pwm_arduino_d6};
  const ra8_port_pin_t d6  = (ra8_port_pin_t)k_ra8_board_arduino_d6;
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_open(pwm, p0, k_test_period, k_fw_pwm_pol_active_high));
  TEST_ASSERT_EQ(k_test_pfs_gpt, pfs_d6() & k_test_pfs_mask);
  TEST_ASSERT_EQ(true, ra8_pin_validator_is_claimed(d6));
  TEST_ASSERT_EQ(k_test_period, ra8_gpt((uint8_t)k_test_ch1)->GTPR);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_start(pwm, p0));
  TEST_ASSERT_EQ(k_test_ch1_bit, ra8_gpt((uint8_t)k_test_ch1)->GTSTR);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_close(pwm, p0));
  TEST_ASSERT_EQ(false, ra8_pin_validator_is_claimed(d6));
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_open(pwm, p0, k_test_period, k_fw_pwm_pol_active_high));
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_close(pwm, p0));
  TEST_END("board PWM 0 routes D6 to GPT1, claims it, releases on close");
}

static void test_pwm_pin_held_elsewhere(void)
{
  TEST_BEGIN("a D6 held by another driver fails the open and leaves GPT1 free");
  reset_state();
  const fw_pwm_t*      pwm = ra8_board_pwm();
  const fw_pwm_ch_t    p0  = {.index = k_ra8_board_pwm_arduino_d6};
  const ra8_port_pin_t d6  = (ra8_port_pin_t)k_ra8_board_arduino_d6;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_claim(d6, "test.other"));
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict,
                 fw_pwm_open(pwm, p0, k_test_period, k_fw_pwm_pol_active_high));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_pwm_close(pwm, p0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pin_validator_release(d6));
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_open(pwm, p0, k_test_period, k_fw_pwm_pol_active_high));
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_close(pwm, p0));
  TEST_END("a D6 held by another driver fails the open and leaves GPT1 free");
}

/** @brief Cases in run order; main walks this so it never grows. */
static void (*const s_test_roster[])(void) = {
  test_lookups,
  test_sets_are_disjoint,
  test_caps_carry_board_counts,
  test_timer_lands_on_its_channel,
  test_pwm_routes_and_releases_its_pin,
  test_pwm_pin_held_elsewhere,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
