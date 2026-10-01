/**
 * @file test_fw_if_pwm_ra8.c
 * @brief Vectors for the RA8 GPT adapter behind the neutral PWM port.
 *
 * @par Tag
 * [Ring 3 / Test] {World: NS}
 *
 * @details
 * Driven through the public `fw_pwm_*` facade, with register state read back
 * from the fake memory map, as in test_fw_if_timer_ra8.c. Channel 7 is the
 * workhorse so a write landing on channel 0 shows. The last vectors bind the
 * timer adapter as well, to prove the two cannot both hold one channel.
 * Every vector closes what it opens: ownership outlives a single case.
 *
 * Register values only; no waveform is proven here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "fw_if_pwm.h"
#include "fw_if_pwm_ra8.h"
#include "fw_if_timer.h"
#include "fw_if_timer_ra8.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_gpt.h"
#include "ra8_gpt_regs.h"
#include "unity_minimal.h"

/** @brief Fixed inputs and the register values they should produce. */
typedef enum : uint32_t {
  k_test_ch           = 7U,           /**< Chip channel under test.          */
  k_test_ch_other     = 3U,           /**< A second channel.                 */
  k_test_ch_bit       = 0x00000080UL, /**< CSTRT7 / CSTOP7.                  */
  k_test_period       = 999U,         /**< 1000 counts per cycle.            */
  k_test_period_long  = 1999U,        /**< 2000 counts per cycle.            */
  k_test_half         = 32768U,       /**< Q16 one half.                     */
  k_test_quarter      = 16384U,       /**< Q16 one quarter.                  */
  k_test_half_cmp     = 500U,         /**< Half of 1000 counts.              */
  k_test_quarter_cmp  = 250U,         /**< Quarter of 1000 counts.           */
  k_test_full_cmp     = 1000U,        /**< Never reached: output stays on.   */
  k_test_long_cmp     = 1000U,        /**< Half of 2000 counts.              */
  k_test_ccr_a        = 0U,           /**< GTCCR index of GTCCRA.            */
  k_test_ccr_c        = 2U,           /**< GTCCR index of GTCCRC (A buffer). */
  k_test_gtior_a_mask = 0x000007FFUL, /**< GTIOA, OADFLT, OAE, OADF.         */
  k_test_gtior_high   = 0x00000109UL, /**< OAE | pattern 0x9, stop low.      */
  k_test_gtior_low    = 0x00000146UL, /**< OAE | OADFLT | pattern 0x6.       */
} test_const_t;

/** @brief A PWM handle bound to the adapter. */
static fw_pwm_t pwm_bound(void)
{
  fw_pwm_t pwm = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_ra8_bind(&pwm));
  return pwm;
}

/** @brief Shorthand for an output by chip channel. */
static fw_pwm_ch_t out_of(uint32_t index)
{
  return (fw_pwm_ch_t){.index = (uint8_t)index};
}

/** @brief Open @p index active-high at ::k_test_period, failing on error. */
static void open_high(const fw_pwm_t* pwm, uint32_t index)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_pwm_open(pwm, out_of(index), k_test_period, k_fw_pwm_pol_active_high));
}

static void test_caps(void)
{
  TEST_BEGIN("caps: ten outputs, 32-bit, period_max one short, active-low");
  const fw_pwm_t pwm  = pwm_bound();
  fw_pwm_caps_t  caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_get_caps(&pwm, &caps));
  TEST_ASSERT_EQ(k_fw_pwm_ra8_channel_count, caps.channel_count);
  TEST_ASSERT_EQ(k_fw_pwm_ra8_counter_bits, caps.counter_bits);
  TEST_ASSERT_EQ(UINT32_MAX - 1U, caps.period_max);
  TEST_ASSERT_EQ(true, caps.has_active_low);
  TEST_END("caps: ten outputs, 32-bit, period_max one short, active-low");
}

static void test_facade_limits(void)
{
  TEST_BEGIN("output 10 not found; a full 32-bit period is out of range");
  const fw_pwm_t pwm = pwm_bound();
  TEST_ASSERT_EQ(
    k_ra8_err_not_found,
    fw_pwm_open(&pwm, out_of(k_fw_pwm_ra8_channel_count), k_test_period, k_fw_pwm_pol_active_high));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 fw_pwm_open(&pwm, out_of(k_test_ch), UINT32_MAX, k_fw_pwm_pol_active_high));
  TEST_END("output 10 not found; a full 32-bit period is out of range");
}

static void test_open_active_high(void)
{
  TEST_BEGIN("open active-high: GTPR, pin A on, stop low, duty zero, stopped");
  ra8_fake_mmap_reset();
  const fw_pwm_t                 pwm = pwm_bound();
  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_test_ch);
  open_high(&pwm, k_test_ch);
  TEST_ASSERT_EQ(k_test_period, reg->GTPR);
  TEST_ASSERT_EQ(k_test_gtior_high, reg->GTIOR & k_test_gtior_a_mask);
  TEST_ASSERT_EQ(0U, reg->GTCCR[k_test_ccr_a]);
  TEST_ASSERT_EQ(0U, reg->GTSTR);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_close(&pwm, out_of(k_test_ch)));
  TEST_END("open active-high: GTPR, pin A on, stop low, duty zero, stopped");
}

static void test_open_active_low(void)
{
  TEST_BEGIN("open active-low: inverted pattern, stop level high");
  ra8_fake_mmap_reset();
  const fw_pwm_t pwm = pwm_bound();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    fw_pwm_open(&pwm, out_of(k_test_ch_other), k_test_period, k_fw_pwm_pol_active_low));
  TEST_ASSERT_EQ(k_test_gtior_low, ra8_gpt((uint8_t)k_test_ch_other)->GTIOR & k_test_gtior_a_mask);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_close(&pwm, out_of(k_test_ch_other)));
  TEST_END("open active-low: inverted pattern, stop level high");
}

static void test_duty_while_stopped(void)
{
  TEST_BEGIN("duty while stopped lands in GTCCRA and its buffer; full never matches");
  ra8_fake_mmap_reset();
  const fw_pwm_t                 pwm = pwm_bound();
  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_test_ch);
  open_high(&pwm, k_test_ch);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_set_duty(&pwm, out_of(k_test_ch), k_test_half));
  TEST_ASSERT_EQ(k_test_half_cmp, reg->GTCCR[k_test_ccr_a]);
  TEST_ASSERT_EQ(k_test_half_cmp, reg->GTCCR[k_test_ccr_c]);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_set_duty(&pwm, out_of(k_test_ch), K_FW_PWM_DUTY_FULL));
  TEST_ASSERT_EQ(k_test_full_cmp, reg->GTCCR[k_test_ccr_a]);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_close(&pwm, out_of(k_test_ch)));
  TEST_END("duty while stopped lands in GTCCRA and its buffer; full never matches");
}

static void test_duty_while_running(void)
{
  TEST_BEGIN("duty while running goes to the buffer only; start/stop hit ch 7");
  ra8_fake_mmap_reset();
  const fw_pwm_t                 pwm = pwm_bound();
  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_test_ch);
  open_high(&pwm, k_test_ch);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_start(&pwm, out_of(k_test_ch)));
  TEST_ASSERT_EQ(k_test_ch_bit, reg->GTSTR);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_set_duty(&pwm, out_of(k_test_ch), k_test_quarter));
  TEST_ASSERT_EQ(k_test_quarter_cmp, reg->GTCCR[k_test_ccr_c]);
  TEST_ASSERT_EQ(0U, reg->GTCCR[k_test_ccr_a]);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_stop(&pwm, out_of(k_test_ch)));
  TEST_ASSERT_EQ(k_test_ch_bit, reg->GTSTP);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_close(&pwm, out_of(k_test_ch)));
  TEST_END("duty while running goes to the buffer only; start/stop hit ch 7");
}

static void test_period_keeps_ratio(void)
{
  TEST_BEGIN("set_period keeps the duty ratio");
  ra8_fake_mmap_reset();
  const fw_pwm_t                 pwm = pwm_bound();
  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_test_ch);
  open_high(&pwm, k_test_ch);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_set_duty(&pwm, out_of(k_test_ch), k_test_half));
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_set_period(&pwm, out_of(k_test_ch), k_test_period_long));
  TEST_ASSERT_EQ(k_test_period_long, reg->GTPR);
  TEST_ASSERT_EQ(k_test_long_cmp, reg->GTCCR[k_test_ccr_a]);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_close(&pwm, out_of(k_test_ch)));
  TEST_END("set_period keeps the duty ratio");
}

static void test_ownership(void)
{
  TEST_BEGIN("double open busy; unopened refused; close frees for reopen");
  ra8_fake_mmap_reset();
  const fw_pwm_t pwm = pwm_bound();
  open_high(&pwm, k_test_ch);
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 fw_pwm_open(&pwm, out_of(k_test_ch), k_test_period, k_fw_pwm_pol_active_low));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_pwm_start(&pwm, out_of(k_test_ch_other)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_pwm_stop(&pwm, out_of(k_test_ch_other)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_pwm_set_duty(&pwm, out_of(k_test_ch_other), 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 fw_pwm_set_period(&pwm, out_of(k_test_ch_other), k_test_period));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_pwm_close(&pwm, out_of(k_test_ch_other)));
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_close(&pwm, out_of(k_test_ch)));
  open_high(&pwm, k_test_ch);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_close(&pwm, out_of(k_test_ch)));
  TEST_END("double open busy; unopened refused; close frees for reopen");
}

static void test_timer_and_pwm_share_nothing(void)
{
  TEST_BEGIN("a channel held by one adapter is busy and closed to the other");
  ra8_fake_mmap_reset();
  const fw_pwm_t pwm = pwm_bound();
  fw_timer_t     tmr = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_ra8_bind(&tmr));
  const fw_timer_ch_t tch = {.index = (uint8_t)k_test_ch};

  open_high(&pwm, k_test_ch);
  TEST_ASSERT_EQ(k_ra8_err_busy, fw_timer_open(&tmr, tch, k_fw_timer_mode_free_run, k_test_period));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_timer_stop(&tmr, tch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_timer_close(&tmr, tch));
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_close(&pwm, out_of(k_test_ch)));

  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_open(&tmr, tch, k_fw_timer_mode_free_run, k_test_period));
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 fw_pwm_open(&pwm, out_of(k_test_ch), k_test_period, k_fw_pwm_pol_active_high));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_pwm_start(&pwm, out_of(k_test_ch)));
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_close(&tmr, tch));
  TEST_END("a channel held by one adapter is busy and closed to the other");
}

/** @brief Cases in run order; main walks this so it never grows. */
static void (*const s_test_roster[])(void) = {
  test_caps,
  test_facade_limits,
  test_open_active_high,
  test_open_active_low,
  test_duty_while_stopped,
  test_duty_while_running,
  test_period_keeps_ratio,
  test_ownership,
  test_timer_and_pwm_share_nothing,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
