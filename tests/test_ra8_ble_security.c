/**
 * @file test_ra8_ble_security.c
 * @brief Unit tests for libs/ra8_ble_host security wrapper.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_ble_security.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum ble_security_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_ble_security_c_ff       = 0xFFU,
  k_ble_security_io_cap_200 = 200U,
} ble_security_uint8_const_t;

/**
 * @enum ble_security_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_ble_security_passkey_123456 = 123456U,
} ble_security_uint32_const_t;

extern void ra8_ble_security_test_emit_event(const ra8_ble_security_event_t* evt);
extern void ra8_ble_security_test_set_bond_count(uint8_t count);

static uint32_t                 s_evt_count;
static ra8_ble_security_event_t s_last;

static void capture_evt(void* ctx, const ra8_ble_security_event_t* evt)
{
  (void)ctx;
  s_evt_count++;
  s_last = *evt;
}

static void reset_state(void)
{
  s_evt_count = 0U;
  memset(&s_last, 0, sizeof(s_last));
  (void)ra8_ble_security_close();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null(void)
{
  TEST_BEGIN("test_init_null");
  reset_state();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ble_security_init(nullptr));
  TEST_END("test_init_null");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_invalid_iocap(void)
{
  TEST_BEGIN("test_init_invalid_iocap");
  reset_state();
  ra8_ble_security_config_t cfg = {};
  cfg.io_cap                    = (ra8_ble_security_io_cap_t)k_ble_security_io_cap_200;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ble_security_init(&cfg));
  TEST_END("test_init_invalid_iocap");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_close_roundtrip(void)
{
  TEST_BEGIN("test_init_close_roundtrip");
  reset_state();
  const ra8_ble_security_config_t cfg = {
    .io_cap           = k_ra8_ble_io_cap_no_input_no_out,
    .bonding_enable   = 1U,
    .mitm_required    = 0U,
    .sc_only          = 0U,
    .use_rsip_offload = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_close());
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ble_security_close());
  TEST_END("test_init_close_roundtrip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_passkey_reply_range(void)
{
  TEST_BEGIN("test_passkey_reply_range");
  reset_state();
  const ra8_ble_security_config_t cfg = {.io_cap = k_ra8_ble_io_cap_keyboard_only};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ble_security_passkey_reply(0x40U, 1000000U, 1U));
  TEST_END("test_passkey_reply_range");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_bond_count(void)
{
  TEST_BEGIN("test_bond_count");
  reset_state();
  uint8_t                         c   = k_ble_security_c_ff;
  const ra8_ble_security_config_t cfg = {.io_cap = k_ra8_ble_io_cap_no_input_no_out};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ble_security_bond_count(&c));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ble_security_bond_count(nullptr));
  ra8_ble_security_test_set_bond_count(2U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_bond_count(&c));
  TEST_ASSERT_EQ(2U, c);
  TEST_END("test_bond_count");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_event_callback(void)
{
  TEST_BEGIN("test_event_callback");
  reset_state();
  const ra8_ble_security_config_t cfg = {.io_cap = k_ra8_ble_io_cap_display_only};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_attach_event_handler(capture_evt, nullptr));
  ra8_ble_security_event_t evt = {.kind    = k_ra8_ble_sec_evt_passkey_display,
                                  .passkey = k_ble_security_passkey_123456};
  ra8_ble_security_test_emit_event(&evt);
  TEST_ASSERT_EQ(1U, s_evt_count);
  TEST_ASSERT_EQ(123456U, s_last.passkey);
  TEST_END("test_event_callback");
}

/* --------------------------------------------------------------------- */
/* MC/DC vector tests for libs/ra8_ble_host/src/ra8_ble_security.c */
/* --------------------------------------------------------------------- */

/**
 * @test test_mcdc_security_emit_event_guard
 *
 * @par MC/DC:
 * Decision: `(s_state.event_fn != NULL) && (evt != NULL)`
 * (2 conditions, ra8_ble_security_test_emit_event in
 * ra8_ble_security.c)
 * Reached via the UNIT_TEST hook ra8_ble_security_test_emit_event().
 * - V1 fn=non-NULL, evt=non-NULL -> C1=T, C2=T. Decision T (callback fires).
 * - V2 fn=NULL,     evt=non-NULL -> C1=F short-circuits. Decision F (no fire).
 * - V3 fn=non-NULL, evt=NULL     -> C1=T, C2=F. Decision F (no fire).
 * V1+V2 vary C1 with C2 held T. V1+V3 vary C2 with C1 held T. N+1=3.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved.
 *
 * @par Internal note (ra8_ble_security.c passkey_reply io_cap arms):
 * The compound `(io_cap==display_yes_no || io_cap==keyboard_display)` in
 * ra8_ble_security_passkey_reply() is wrapped in #ifdef RA8_TARGET_BUILD
 * and is dead in the host (UNIT_TEST) build. Per IEC 61508 / DO-178C
 * 6.4.4.3 the host harness has no execution path into that branch;
 * coverage is taken on the target build via the integrated NimBLE SM
 * passkey-display fixture.
 */
static void test_mcdc_security_emit_event_guard(void)
{
  TEST_BEGIN("mcdc security emit_event (fn!=NULL && evt!=NULL)");
  reset_state();
  s_evt_count                         = 0U;
  const ra8_ble_security_config_t cfg = {.io_cap = k_ra8_ble_io_cap_display_only};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_init(&cfg));
  ra8_ble_security_event_t evt = {.kind = k_ra8_ble_sec_evt_passkey_display, .passkey = 1U};
  /* V1: handler registered, evt non-NULL -> fires. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_attach_event_handler(capture_evt, nullptr));
  ra8_ble_security_test_emit_event(&evt);
  TEST_ASSERT_EQ(1U, s_evt_count);
  /* V2: handler NULL, evt non-NULL -> no fire. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_attach_event_handler(nullptr, nullptr));
  ra8_ble_security_test_emit_event(&evt);
  TEST_ASSERT_EQ(1U, s_evt_count);
  /* V3: handler registered again, evt NULL -> no fire. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_attach_event_handler(capture_evt, nullptr));
  ra8_ble_security_test_emit_event(nullptr);
  TEST_ASSERT_EQ(1U, s_evt_count);
  TEST_END("mcdc security emit_event (fn!=NULL && evt!=NULL)");
}

int main(void)
{
  test_init_null();
  test_init_invalid_iocap();
  test_init_close_roundtrip();
  test_passkey_reply_range();
  test_bond_count();
  test_event_callback();
  test_mcdc_security_emit_event_guard();
  return 0;
}
