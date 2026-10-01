/**
 * @file test_fw_if_pwm.c
 * @brief Host vectors for the `fw_pwm` port.
 *
 * @par Tag
 * [Ring 3 / Test] {World: NS}
 *
 * @details
 * No chip adapter yet, so these drive the facade through a fake binding whose
 * caps each vector sets. What is proved is facade behaviour no chip can
 * change: the entry guards, malformed-binding rejection, the caps snapshot,
 * the period check against the counter, the polarity check, and the Q16 duty
 * ceiling, which is inclusive so that full-on is expressible exactly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fw_if_pwm.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief Counter limits the vectors pin their expectations to. */
typedef enum : uint32_t {
  k_fake_max_16bit = 65535U,      /**< Widest 16-bit period. */
  k_fake_max_32bit = 4294967295U, /**< Widest 32-bit period. */
} fake_limit_t;

/** @brief Channel count and widths used by the vectors. */
typedef enum : uint8_t {
  k_fake_channels = 4U,  /**< Board outputs the fake reports. */
  k_fake_bits_16  = 16U, /**< Narrow counter width.           */
  k_fake_bits_32  = 32U, /**< Wide counter width.             */
} fake_shape_t;

/** @brief What the fake binding was asked, so a vector can assert on it. */
typedef struct {
  fw_pwm_caps_t     caps;        /**< What get_caps reports.           */
  ra8_err_t         caps_err;    /**< Error get_caps reports.          */
  ra8_err_t         op_err;      /**< Error the forwarding ops report. */
  fw_pwm_polarity_t last_pol;    /**< Polarity of the last open.       */
  uint32_t          last_period; /**< Period of the last open/retune.  */
  fw_pwm_duty_t     last_duty;   /**< Duty of the last set_duty.       */
  uint8_t           last_index;  /**< Output of the last call.         */
  uint32_t          calls;       /**< Total forwarding ops entered.    */
} fake_state_t;

static fake_state_t g_fake;

static ra8_err_t fake_get_caps(void* ctx, fw_pwm_caps_t* out)
{
  fake_state_t* st = (fake_state_t*)ctx;
  *out             = st->caps;
  return st->caps_err;
}

static ra8_err_t fake_open(void* ctx, fw_pwm_ch_t ch, uint32_t period, fw_pwm_polarity_t pol)
{
  fake_state_t* st = (fake_state_t*)ctx;
  st->calls += 1U;
  st->last_index  = ch.index;
  st->last_period = period;
  st->last_pol    = pol;
  return st->op_err;
}

static ra8_err_t fake_plain(void* ctx, fw_pwm_ch_t ch)
{
  fake_state_t* st = (fake_state_t*)ctx;
  st->calls += 1U;
  st->last_index = ch.index;
  return st->op_err;
}

static ra8_err_t fake_set_period(void* ctx, fw_pwm_ch_t ch, uint32_t period)
{
  fake_state_t* st = (fake_state_t*)ctx;
  st->calls += 1U;
  st->last_index  = ch.index;
  st->last_period = period;
  return st->op_err;
}

static ra8_err_t fake_set_duty(void* ctx, fw_pwm_ch_t ch, fw_pwm_duty_t duty)
{
  fake_state_t* st = (fake_state_t*)ctx;
  st->calls += 1U;
  st->last_index = ch.index;
  st->last_duty  = duty;
  return st->op_err;
}

static const fw_pwm_iface_t g_fake_iface = {
  .get_caps   = fake_get_caps,
  .open       = fake_open,
  .close      = fake_plain,
  .start      = fake_plain,
  .stop       = fake_plain,
  .set_period = fake_set_period,
  .set_duty   = fake_set_duty,
};

static const fw_pwm_ch_t k_ch0 = {.index = 0U};

/** @brief Reset the fake to a 32-bit, four-output, invertible backend. */
static void internal_fake_reset_32bit(void)
{
  const fake_state_t fresh = {
    .caps =
      {
        .channel_count  = (uint8_t)k_fake_channels,
        .counter_bits   = (uint8_t)k_fake_bits_32,
        .period_max     = (uint32_t)k_fake_max_32bit,
        .has_active_low = true,
      },
    .caps_err = k_ra8_ok,
    .op_err   = k_ra8_ok,
  };
  g_fake = fresh;
}

/** @brief Bind @p pwm to the shared fake, asserting it binds. */
static void internal_bind(fw_pwm_t* pwm)
{
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_bind(pwm, &g_fake_iface, &g_fake));
}

static void test_bind_rejects_null_and_incomplete(void)
{
  TEST_BEGIN("bind rejects a NULL handle, NULL ops, and any single unset op");

  internal_fake_reset_32bit();
  fw_pwm_t pwm = {0};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_pwm_bind(nullptr, &g_fake_iface, &g_fake));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_pwm_bind(&pwm, nullptr, &g_fake));

  for (uint32_t hole = 0U; hole < 7U; ++hole) {
    fw_pwm_iface_t broken = g_fake_iface;
    switch (hole) {
      case 0U:
        broken.get_caps = nullptr;
        break;
      case 1U:
        broken.open = nullptr;
        break;
      case 2U:
        broken.close = nullptr;
        break;
      case 3U:
        broken.start = nullptr;
        break;
      case 4U:
        broken.stop = nullptr;
        break;
      case 5U:
        broken.set_period = nullptr;
        break;
      default:
        broken.set_duty = nullptr;
        break;
    }
    fw_pwm_t h = {0};
    TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_pwm_bind(&h, &broken, &g_fake));
    TEST_ASSERT_EQ(false, h.bound);
  }

  TEST_END("bind rejects a NULL handle, NULL ops, and any single unset op");
}

static void test_bind_refuses_unusable_caps(void)
{
  TEST_BEGIN("a backend reporting a zero counter width or period_max is refused at bind");

  internal_fake_reset_32bit();
  g_fake.caps.counter_bits = 0U;
  fw_pwm_t pwm             = {0};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_pwm_bind(&pwm, &g_fake_iface, &g_fake));

  internal_fake_reset_32bit();
  g_fake.caps.period_max = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_pwm_bind(&pwm, &g_fake_iface, &g_fake));

  internal_fake_reset_32bit();
  g_fake.caps_err = k_ra8_err_hw_error;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, fw_pwm_bind(&pwm, &g_fake_iface, &g_fake));
  TEST_ASSERT_EQ(false, pwm.bound);

  TEST_END("a backend reporting a zero counter width or period_max is refused at bind");
}

static void test_unbound_handle_is_refused(void)
{
  TEST_BEGIN("every entry point refuses a zeroed handle rather than jumping through it");

  internal_fake_reset_32bit();
  const fw_pwm_t zero = {0};
  fw_pwm_caps_t  caps = {.channel_count = 9U};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_pwm_get_caps(&zero, &caps));
  TEST_ASSERT_EQ(0U, caps.channel_count);
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 fw_pwm_open(&zero, k_ch0, 100U, k_fw_pwm_pol_active_high));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_pwm_close(&zero, k_ch0));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_pwm_start(&zero, k_ch0));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_pwm_stop(&zero, k_ch0));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_pwm_set_period(&zero, k_ch0, 100U));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_pwm_set_duty(&zero, k_ch0, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_pwm_start(nullptr, k_ch0));
  TEST_ASSERT_EQ(0U, g_fake.calls);

  TEST_END("every entry point refuses a zeroed handle rather than jumping through it");
}

static void test_caps_snapshot_survives_a_lying_backend(void)
{
  TEST_BEGIN("caps come from the bind-time snapshot, not a fresh call to the backend");

  internal_fake_reset_32bit();
  fw_pwm_t pwm = {0};
  internal_bind(&pwm);
  g_fake.caps.channel_count = 1U;
  g_fake.caps.period_max    = 10U;

  fw_pwm_caps_t caps = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_get_caps(&pwm, &caps));
  TEST_ASSERT_EQ((uint8_t)k_fake_channels, caps.channel_count);
  TEST_ASSERT_EQ((uint32_t)k_fake_max_32bit, caps.period_max);

  TEST_END("caps come from the bind-time snapshot, not a fresh call to the backend");
}

static void test_period_is_range_checked_against_the_counter(void)
{
  TEST_BEGIN("a period wider than the counter is refused instead of truncating");

  internal_fake_reset_32bit();
  g_fake.caps.counter_bits = (uint8_t)k_fake_bits_16;
  g_fake.caps.period_max   = (uint32_t)k_fake_max_16bit;
  fw_pwm_t pwm             = {0};
  internal_bind(&pwm);

  TEST_ASSERT_EQ(
    k_ra8_err_out_of_range,
    fw_pwm_open(&pwm, k_ch0, (uint32_t)k_fake_max_16bit + 1U, k_fw_pwm_pol_active_high));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_pwm_open(&pwm, k_ch0, 0U, k_fw_pwm_pol_active_high));
  TEST_ASSERT_EQ(0U, g_fake.calls);
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_pwm_open(&pwm, k_ch0, (uint32_t)k_fake_max_16bit, k_fw_pwm_pol_active_high));
  TEST_ASSERT_EQ((uint32_t)k_fake_max_16bit, g_fake.last_period);

  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 fw_pwm_set_period(&pwm, k_ch0, (uint32_t)k_fake_max_16bit + 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_pwm_set_period(&pwm, k_ch0, 0U));
  TEST_ASSERT_EQ(1U, g_fake.calls);

  TEST_END("a period wider than the counter is refused instead of truncating");
}

static void test_polarity_is_enumerated_and_capability_checked(void)
{
  TEST_BEGIN("an unenumerated polarity is an argument error, an absent one is not supported");

  internal_fake_reset_32bit();
  g_fake.caps.has_active_low = false;
  fw_pwm_t pwm               = {0};
  internal_bind(&pwm);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_pwm_open(&pwm, k_ch0, 100U, k_fw_pwm_pol_none));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_pwm_open(&pwm, k_ch0, 100U, (fw_pwm_polarity_t)K_FW_PWM_POL_COUNT));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, fw_pwm_open(&pwm, k_ch0, 100U, k_fw_pwm_pol_active_low));
  TEST_ASSERT_EQ(0U, g_fake.calls);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_open(&pwm, k_ch0, 100U, k_fw_pwm_pol_active_high));
  TEST_ASSERT_EQ(k_fw_pwm_pol_active_high, g_fake.last_pol);

  TEST_END("an unenumerated polarity is an argument error, an absent one is not supported");
}

static void test_duty_ceiling_is_inclusive_full_scale(void)
{
  TEST_BEGIN("duty 0 and full scale both pass, one above full scale is refused");

  internal_fake_reset_32bit();
  fw_pwm_t pwm = {0};
  internal_bind(&pwm);

  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_set_duty(&pwm, k_ch0, 0U));
  TEST_ASSERT_EQ(0U, g_fake.last_duty);
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_set_duty(&pwm, k_ch0, (fw_pwm_duty_t)K_FW_PWM_DUTY_FULL));
  TEST_ASSERT_EQ((fw_pwm_duty_t)K_FW_PWM_DUTY_FULL, g_fake.last_duty);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 fw_pwm_set_duty(&pwm, k_ch0, (fw_pwm_duty_t)K_FW_PWM_DUTY_FULL + 1U));
  TEST_ASSERT_EQ(2U, g_fake.calls);

  TEST_END("duty 0 and full scale both pass, one above full scale is refused");
}

static void test_channel_index_is_checked_against_the_board_count(void)
{
  TEST_BEGIN("an output the board does not carry is not_found on every operation");

  internal_fake_reset_32bit();
  fw_pwm_t pwm = {0};
  internal_bind(&pwm);
  const fw_pwm_ch_t past = {.index = (uint8_t)k_fake_channels};

  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_pwm_open(&pwm, past, 100U, k_fw_pwm_pol_active_high));
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_pwm_close(&pwm, past));
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_pwm_start(&pwm, past));
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_pwm_stop(&pwm, past));
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_pwm_set_period(&pwm, past, 100U));
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_pwm_set_duty(&pwm, past, 0U));
  TEST_ASSERT_EQ(0U, g_fake.calls);

  TEST_END("an output the board does not carry is not_found on every operation");
}

static void test_backend_errors_propagate(void)
{
  TEST_BEGIN("a backend error on any forwarded op reaches the caller unchanged");

  internal_fake_reset_32bit();
  fw_pwm_t pwm = {0};
  internal_bind(&pwm);
  g_fake.op_err = k_ra8_err_busy;

  TEST_ASSERT_EQ(k_ra8_err_busy, fw_pwm_open(&pwm, k_ch0, 100U, k_fw_pwm_pol_active_high));
  TEST_ASSERT_EQ(k_ra8_err_busy, fw_pwm_close(&pwm, k_ch0));
  TEST_ASSERT_EQ(k_ra8_err_busy, fw_pwm_start(&pwm, k_ch0));
  TEST_ASSERT_EQ(k_ra8_err_busy, fw_pwm_stop(&pwm, k_ch0));
  TEST_ASSERT_EQ(k_ra8_err_busy, fw_pwm_set_period(&pwm, k_ch0, 100U));
  TEST_ASSERT_EQ(k_ra8_err_busy, fw_pwm_set_duty(&pwm, k_ch0, 1U));
  TEST_ASSERT_EQ(6U, g_fake.calls);

  TEST_END("a backend error on any forwarded op reaches the caller unchanged");
}

static void test_context_reaches_every_op(void)
{
  TEST_BEGIN("the binding context is handed back to every op, so no file-scope state");

  internal_fake_reset_32bit();
  fake_state_t first  = g_fake;
  fake_state_t second = g_fake;

  fw_pwm_t pwm_a = {0};
  fw_pwm_t pwm_b = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_bind(&pwm_a, &g_fake_iface, &first));
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_bind(&pwm_b, &g_fake_iface, &second));

  const fw_pwm_ch_t ch2 = {.index = 2U};
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_set_duty(&pwm_a, k_ch0, 7U));
  TEST_ASSERT_EQ(k_ra8_ok, fw_pwm_stop(&pwm_b, ch2));
  TEST_ASSERT_EQ(1U, first.calls);
  TEST_ASSERT_EQ(1U, second.calls);
  TEST_ASSERT_EQ(7U, first.last_duty);
  TEST_ASSERT_EQ(2U, second.last_index);

  TEST_END("the binding context is handed back to every op, so no file-scope state");
}

int main(void)
{
  test_bind_rejects_null_and_incomplete();
  test_bind_refuses_unusable_caps();
  test_unbound_handle_is_refused();
  test_caps_snapshot_survives_a_lying_backend();
  test_period_is_range_checked_against_the_counter();
  test_polarity_is_enumerated_and_capability_checked();
  test_duty_ceiling_is_inclusive_full_scale();
  test_channel_index_is_checked_against_the_board_count();
  test_backend_errors_propagate();
  test_context_reaches_every_op();
  return 0;
}
