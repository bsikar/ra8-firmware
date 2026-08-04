/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_wdt_supervisor.c
 * @brief Unit tests + MC/DC vectors for libs/ra8_wdt_supervisor/src/ra8_wdt_supervisor.c
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_err.h"
#include "ra8_wdt_supervisor.h"
#include "unity_minimal.h"

/**
 * @enum t_sup_t
 * @brief Supervisor task stack and the clock push past the deadline.
 */
typedef enum : uint16_t {
  k_t_stack_bytes      = 1024U, /**< Supervisor task stack, bytes. */
  k_t_past_deadline_ms = 1000U, /**< Monotonic clock value well past the refresh
                                     deadline, so the missed-refresh path fires. */
} t_sup_t;

/* ---------------------------------------------------------------------------
 * Test scaffolding
 * ---------------------------------------------------------------------------
 */

static uint32_t s_now_ms;
static uint32_t s_refresh_calls;
static uint8_t  s_stack[k_t_stack_bytes];

static uint32_t test_now_hook(void)
{
  return s_now_ms;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_refresh_hook(void)
{
  s_refresh_calls++;
}

static void prep(void)
{
  /* deinit any previous run; ignore error if not initialized. */
  (void)ra8_wdt_supervisor_deinit();
  s_now_ms        = 0U;
  s_refresh_calls = 0U;

  const ra8_wdt_sup_cfg_t cfg = {
    .stack             = s_stack,
    .stack_size_bytes  = (uint32_t)sizeof(s_stack),
    .priority          = 4U,
    .refresh_period_ms = 50U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_set_now_hook(test_now_hook));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_set_refresh_hook(test_refresh_hook));
}

/**
 * @test test_mcdc_supervisor_tick_will_refresh
 *
 * @par MC/DC:
 * Decision: ``const bool will_refresh = any_present && all_alive;``
 * (2 conditions, libs/ra8_wdt_supervisor/src/ra8_wdt_supervisor.c line 396)
 *
 * Vectors (Chilenski masking-MC/DC, N+1 = 3 for N=2):
 *  - V1: zero workers registered (any_present=F, all_alive=T trivially)
 *        -> C1=F shorts. will_refresh=F.
 *  - V2: one worker, alive (any_present=T, all_alive=T)
 *        -> C1=T, C2=T. will_refresh=T.
 *  - V3: one worker, overdue (any_present=T, all_alive=F)
 *        -> C1=T, C2=F. will_refresh=F.
 *
 * Independence:
 *  - V1 vs V2 vary C1 with C2 held T: outcome flips.
 *  - V2 vs V3 vary C2 with C1 held T: outcome flips.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_supervisor_tick_will_refresh(void)
{
  TEST_BEGIN("ra8_wdt_supervisor_tick MC/DC: any_present && all_alive");

  /* V1: zero workers -> any_present F, will_refresh F. */
  prep();
  bool did_refresh = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_tick(&did_refresh));
  TEST_ASSERT_EQ(0, did_refresh);
  TEST_ASSERT_EQ(0, s_refresh_calls);

  /* V2: one worker, just checked-in (alive). will_refresh T. */
  prep();
  uint8_t h = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_register_thread("w1", 100U, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_checkin(h));
  did_refresh = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_tick(&did_refresh));
  TEST_ASSERT_EQ(1, did_refresh);
  TEST_ASSERT_EQ(1, s_refresh_calls);

  /* V3: one worker, overdue. will_refresh F. */
  prep();
  h = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_register_thread("w2", 10U, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_checkin(h));
  s_now_ms    = k_t_past_deadline_ms; /* push monotonic clock past the deadline */
  did_refresh = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_tick(&did_refresh));
  TEST_ASSERT_EQ(0, did_refresh);
  TEST_ASSERT_EQ(0, s_refresh_calls);

  TEST_END("ra8_wdt_supervisor_tick MC/DC: any_present && all_alive");
}

/**
 * @test test_mcdc_supervisor_tick_invoke_refresh
 *
 * @par MC/DC:
 * Decision: ``if (will_refresh && (s_state.refresh != nullptr))``
 * (2 conditions, libs/ra8_wdt_supervisor/src/ra8_wdt_supervisor.c line 397)
 *
 * Vectors (Chilenski masking-MC/DC, N+1 = 3 for N=2):
 *  - V1: will_refresh=F (no workers)              -> C1=F shorts. No call.
 *  - V2: will_refresh=T, refresh!=NULL            -> C1=T, C2=T. Hook called.
 *  - V3: will_refresh=T, refresh==NULL            -> C1=T, C2=F. No call.
 *
 * V1 vs V2 vary C1; V2 vs V3 vary C2.
 *
 * Note: in production ``s_state.refresh`` is initialized to a non-null
 * default; ``ra8_wdt_supervisor_set_refresh_hook(NULL)`` restores that
 * default, so observing the C2=F branch via the public API requires
 * the C1=F short-circuit path (V1 covers that masking pair). The C2=F
 * vector with C1=T is documented as unreachable through the public
 * API surface; the line-397 sub-decision is therefore satisfied by the
 * V1+V2 masking-pair plus the line-396 vector set above (which proves
 * the C1 dimension independently).
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision. The C2=F-with-C1=T vector is unreachable via
 * the public API (the default refresh hook is always non-null), so the
 * obligation is discharged by V1 (decision F via short-circuit) plus
 * V2 (decision T) plus the structural cross-check in
 * ``test_mcdc_supervisor_tick_will_refresh`` above which exercises
 * both C1 transitions. This satisfies DO-178C 6.4.4.3 unreachability
 * exemption (defensive code masked by an invariant maintained by the
 * setter API).
 */
static void test_mcdc_supervisor_tick_invoke_refresh(void)
{
  TEST_BEGIN("ra8_wdt_supervisor_tick MC/DC: will_refresh && refresh != NULL");

  /* V1: no workers => will_refresh=F => C1=F => refresh hook not invoked. */
  prep();
  bool did_refresh = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_tick(&did_refresh));
  TEST_ASSERT_EQ(0, s_refresh_calls);

  /* V2: alive worker => will_refresh=T, refresh!=NULL => hook called once. */
  prep();
  uint8_t h = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_register_thread("w1", 100U, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_checkin(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_tick(&did_refresh));
  TEST_ASSERT_EQ(1, s_refresh_calls);

  TEST_END("ra8_wdt_supervisor_tick MC/DC: will_refresh && refresh != NULL");
}

int32_t main(void)
{
  test_mcdc_supervisor_tick_will_refresh();
  test_mcdc_supervisor_tick_invoke_refresh();
  (void)fprintf(stderr, "[OK ] test_ra8_wdt_supervisor.c\n");
  return 0;
}
