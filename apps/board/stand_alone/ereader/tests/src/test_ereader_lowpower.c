/**
 * @file test_ereader_lowpower.c
 * @brief Host unit and MC/DC tests for `er_lowpower` dual-core M33 hold policy.
 *
 * @details
 * Exercises ``er_lowpower_init``, ``er_lowpower_set_enabled``,
 * ``er_lowpower_toggle``, ``er_lowpower_request_handoff``,
 * ``er_lowpower_tick_inputs``, ``er_lowpower_enter_hold``, and read helpers
 * across valid transitions, error paths, SW2 short-vs-long differentiation,
 * repeated multi-turn M85/M33 handoffs, and M33-initiated low-power disable.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "er_lowpower.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief Simulated M33/NSC state shared with mock platform callbacks. */
typedef struct {
  er_lp_mailbox_t     mbox;
  uint32_t            start_calls;
  uint32_t            park_calls;
  uint32_t            notify_calls;
  uint32_t            before_park_calls;
  uint32_t            after_wake_calls;
  ra8_err_t           next_start_err;
  ra8_err_t           next_park_err;
  bool                stamp_ready_on_start;
  er_lp_wake_reason_t scripted_wake;
} t_lp_sim_t;

static t_lp_sim_t s_sim;

static ra8_err_t mock_start_m33(uintptr_t mram_base, uintptr_t stack_top)
{
  s_sim.start_calls += 1U;
  if ((mram_base != k_er_lp_cpu1_mram_base) || (stack_top != k_er_lp_cpu1_stack_top)) {
    return k_ra8_err_invalid_arg;
  }
  if (s_sim.next_start_err != k_ra8_ok) {
    return s_sim.next_start_err;
  }
  if (s_sim.stamp_ready_on_start) {
    s_sim.mbox.m33_sig = (uint32_t)k_er_lp_m33_ready_sig;
  }
  return k_ra8_ok;
}

static ra8_err_t mock_park_m85(uint32_t max_wake_iters, uint32_t* out_woken)
{
  (void)max_wake_iters;
  s_sim.park_calls += 1U;
  if (s_sim.next_park_err != k_ra8_ok) {
    return s_sim.next_park_err;
  }
  if (s_sim.scripted_wake != k_er_lp_wake_none) {
    s_sim.mbox.wake_reason = (uint32_t)s_sim.scripted_wake;
    s_sim.mbox.turn_req += 1U;
    s_sim.mbox.m33_heartbeat += 5U;
    *out_woken = 1U;
  } else {
    *out_woken = 0U;
  }
  return k_ra8_ok;
}

static ra8_err_t mock_notify_cpu1(void)
{
  s_sim.notify_calls += 1U;
  return k_ra8_ok;
}

static void mock_before_park(void)
{
  s_sim.before_park_calls += 1U;
}

static void mock_after_wake(void)
{
  s_sim.after_wake_calls += 1U;
}

static er_lp_config_t default_cfg(bool start_enabled)
{
  return (er_lp_config_t){
    .mailbox            = &s_sim.mbox,
    .ops                = {.start_m33   = mock_start_m33,
                           .park_m85    = mock_park_m85,
                           .notify_cpu1 = mock_notify_cpu1,
                           .before_park = mock_before_park,
                           .after_wake  = mock_after_wake},
    .idle_threshold_ms  = 100U,
    .sw2_toggle_hold_ms = 500U,
    .max_park_iters     = 16U,
    .auto_turn_polls    = 0U,
    .initial_fb_base    = 0x68000000UL,
    .start_enabled      = start_enabled,
  };
}

RA8_INTERNAL static void reset_sim(bool stamp_ready)
{
  s_sim                      = (t_lp_sim_t){};
  s_sim.stamp_ready_on_start = stamp_ready;
  s_sim.scripted_wake        = k_er_lp_wake_next_page;
}

/** @brief Precondition, uninitialized, and start_m33 failure guards. */
RA8_INTERNAL static void test_init_and_guards(void)
{
  TEST_BEGIN("er_lowpower: init and guards");
  er_lp_ctx_t          ctx = {};
  er_lp_input_action_t act = k_er_lp_action_none;
  er_lp_wake_reason_t  wk  = k_er_lp_wake_none;

  reset_sim(true);
  TEST_ASSERT(!er_lowpower_is_enabled(nullptr));
  TEST_ASSERT_EQ(k_er_lp_state_uninit, er_lowpower_state(nullptr));
  TEST_ASSERT_EQ(0U, er_lowpower_handoff_count(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, er_lowpower_set_enabled(&ctx, true));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, er_lowpower_toggle(&ctx, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, er_lowpower_request_handoff(&ctx));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 er_lowpower_tick_inputs(&ctx, false, false, 16U, false, &act));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 er_lowpower_enter_hold(&ctx, 1U, 0x68000000UL, 0x1234U, &wk));

  er_lp_config_t cfg = default_cfg(true);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, er_lowpower_init(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, er_lowpower_init(&ctx, nullptr));
  cfg.max_park_iters = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, er_lowpower_init(&ctx, &cfg));
  cfg.max_park_iters = 16U;

  s_sim.next_start_err = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, er_lowpower_init(&ctx, &cfg));
  TEST_ASSERT_EQ(k_er_lp_state_uninit, er_lowpower_state(&ctx));
  TEST_END("er_lowpower: init and guards");
}

/** @brief Verify SW2 short tap, SW2 long hold toggle, and idle handoff gating. */
RA8_INTERNAL static void test_sw2_short_vs_long_and_idle(void)
{
  TEST_BEGIN("er_lowpower: SW2 short vs long and idle handoff");
  reset_sim(true);
  er_lp_ctx_t          ctx = {};
  er_lp_config_t       cfg = default_cfg(true);
  er_lp_input_action_t act = k_er_lp_action_none;

  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_init(&ctx, &cfg));
  TEST_ASSERT_EQ((uint32_t)k_er_lp_mailbox_magic, s_sim.mbox.magic);
  TEST_ASSERT(er_lowpower_is_enabled(&ctx));

  /* Short SW2 press (< 500ms) followed by release emits sw2_short and preserves enabled. */
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_tick_inputs(&ctx, false, true, 100U, false, &act));
  TEST_ASSERT_EQ(k_er_lp_action_none, act);
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_tick_inputs(&ctx, false, false, 16U, false, &act));
  TEST_ASSERT_EQ(k_er_lp_action_sw2_short, act);
  TEST_ASSERT(er_lowpower_is_enabled(&ctx));

  /* Long SW2 hold (>= 500ms) toggles LP off immediately and suppresses short on release. */
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_tick_inputs(&ctx, false, true, 250U, false, &act));
  TEST_ASSERT_EQ(k_er_lp_action_none, act);
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_tick_inputs(&ctx, false, true, 260U, false, &act));
  TEST_ASSERT_EQ(k_er_lp_action_toggled, act);
  TEST_ASSERT(!er_lowpower_is_enabled(&ctx));
  TEST_ASSERT_EQ(k_er_lp_state_disabled, er_lowpower_state(&ctx));
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_tick_inputs(&ctx, false, false, 16U, false, &act));
  TEST_ASSERT_EQ(k_er_lp_action_none, act);

  /* Explicit handoff fails while disabled, works after toggle back ON. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, er_lowpower_request_handoff(&ctx));
  bool now_on = false;
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_toggle(&ctx, &now_on));
  TEST_ASSERT(now_on);

  /* Activity resets idle timer; 50ms + 50ms idle reaches 100ms threshold -> enter_hold. */
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_tick_inputs(&ctx, true, false, 60U, true, &act));
  TEST_ASSERT_EQ(k_er_lp_action_none, act);
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_tick_inputs(&ctx, false, false, 50U, false, &act));
  TEST_ASSERT_EQ(k_er_lp_action_none, act);
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_tick_inputs(&ctx, false, false, 50U, false, &act));
  TEST_ASSERT_EQ(k_er_lp_action_enter_hold, act);
  TEST_END("er_lowpower: SW2 short vs long and idle handoff");
}

/** @brief Verify repeated page-turn handoffs and M33 SW2-toggle disable path. */
RA8_INTERNAL static void test_repeated_handoffs_and_m33_toggle(void)
{
  TEST_BEGIN("er_lowpower: repeated handoffs and M33 toggle");
  reset_sim(false); /* Start before M33 stamps m33_sig. */
  er_lp_ctx_t         ctx = {};
  er_lp_config_t      cfg = default_cfg(true);
  er_lp_wake_reason_t wk  = k_er_lp_wake_none;

  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_init(&ctx, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 er_lowpower_enter_hold(&ctx, 0U, 0x68000000UL, 0x1111U, &wk));

  s_sim.mbox.m33_sig = (uint32_t)k_er_lp_m33_ready_sig;
  for (uint32_t page = 0U; page < 4U; ++page) {
    TEST_ASSERT_EQ(
      k_ra8_ok,
      er_lowpower_enter_hold(&ctx, page, 0x68000000UL + (page * 4U), 0xA000U + page, &wk));
    TEST_ASSERT_EQ(k_er_lp_wake_next_page, wk);
    TEST_ASSERT_EQ(s_sim.mbox.turn_req, s_sim.mbox.turn_ack);
    TEST_ASSERT_EQ(page, s_sim.mbox.held_page);
    TEST_ASSERT(er_lowpower_is_enabled(&ctx));
  }
  TEST_ASSERT_EQ(4U, er_lowpower_handoff_count(&ctx));
  TEST_ASSERT_EQ(4U, s_sim.before_park_calls);
  TEST_ASSERT_EQ(4U, s_sim.after_wake_calls);

  /* M33 waking with wake_toggle disables low-power mode cleanly. */
  s_sim.scripted_wake = k_er_lp_wake_toggle;
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_enter_hold(&ctx, 4U, 0x68000000UL, 0xA004U, &wk));
  TEST_ASSERT_EQ(k_er_lp_wake_toggle, wk);
  TEST_ASSERT(!er_lowpower_is_enabled(&ctx));
  TEST_ASSERT_EQ(k_er_lp_state_disabled, er_lowpower_state(&ctx));
  TEST_ASSERT_EQ((uint32_t)k_er_lp_status_disabled, s_sim.mbox.status);
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 er_lowpower_enter_hold(&ctx, 4U, 0x68000000UL, 0xA004U, &wk));
  TEST_END("er_lowpower: repeated handoffs and M33 toggle");
}

/** @brief Verify screen-off idle handoff request, input activity cancellation, and wake. */
RA8_INTERNAL static void test_screen_off_idle_handoff_and_wake(void)
{
  TEST_BEGIN("er_lowpower: screen-off idle handoff request and wake");
  reset_sim(true);
  er_lp_ctx_t          ctx = {};
  er_lp_config_t       cfg = default_cfg(true);
  er_lp_input_action_t act = k_er_lp_action_none;
  er_lp_wake_reason_t  wk  = k_er_lp_wake_none;

  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_init(&ctx, &cfg));
  TEST_ASSERT(er_lowpower_is_enabled(&ctx));

  /* Calling request_handoff directly triggers enter_hold on the very next tick. */
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_request_handoff(&ctx));
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_tick_inputs(&ctx, false, false, 16U, false, &act));
  TEST_ASSERT_EQ(k_er_lp_action_enter_hold, act);

  /* Enter hold and verify clean wake by M33. */
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_enter_hold(&ctx, 2U, 0x68000000UL, 0xBEEFU, &wk));
  TEST_ASSERT_EQ(k_er_lp_wake_next_page, wk);
  TEST_ASSERT_EQ(1U, er_lowpower_handoff_count(&ctx));

  /* Request handoff, but cancel it via touch/user activity before enter_hold runs. */
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_request_handoff(&ctx));
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_tick_inputs(&ctx, false, false, 16U, true, &act));
  TEST_ASSERT_EQ(k_er_lp_action_none, act);

  /* Next tick without activity must not enter hold because handoff was cancelled and idle < threshold. */
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_tick_inputs(&ctx, false, false, 16U, false, &act));
  TEST_ASSERT_EQ(k_er_lp_action_none, act);

  /* Request handoff, but cancel it via button down (SW1). */
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_request_handoff(&ctx));
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_tick_inputs(&ctx, true, false, 16U, false, &act));
  TEST_ASSERT_EQ(k_er_lp_action_none, act);

  /* Disable low-power mode and verify request_handoff is rejected. */
  TEST_ASSERT_EQ(k_ra8_ok, er_lowpower_set_enabled(&ctx, false));
  TEST_ASSERT(!er_lowpower_is_enabled(&ctx));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, er_lowpower_request_handoff(&ctx));
  TEST_END("er_lowpower: screen-off idle handoff request and wake");
}

int main(void)
{
  test_init_and_guards();
  test_sw2_short_vs_long_and_idle();
  test_repeated_handoffs_and_m33_toggle();
  test_screen_off_idle_handoff_and_wake();
  return 0;
}
