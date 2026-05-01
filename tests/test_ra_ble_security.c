/**
 * @file test_ra_ble_security.c
 * @brief Unit tests for libs/ra_ble_host security wrapper.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_ble_security.h"
#include "ra_err.h"
#include "unity_minimal.h"

extern void ra_ble_security_test_emit_event(const ra_ble_security_event_t* evt);
extern void ra_ble_security_test_set_bond_count(uint8_t count);

static uint32_t                s_evt_count;
static ra_ble_security_event_t s_last;

static void capture_evt(void* ctx, const ra_ble_security_event_t* evt)
{
  (void)ctx;
  s_evt_count++;
  s_last = *evt;
}

static void reset_state(void)
{
  s_evt_count = 0U;
  memset(&s_last, 0, sizeof(s_last));
  (void)ra_ble_security_close();
}

static void test_init_null(void)
{
  TEST_BEGIN("test_init_null");
  reset_state();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_ble_security_init(NULL));
  TEST_END("test_init_null");
}

static void test_init_invalid_iocap(void)
{
  TEST_BEGIN("test_init_invalid_iocap");
  reset_state();
  ra_ble_security_config_t cfg = {};
  cfg.io_cap                   = (ra_ble_security_io_cap_t)200U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ble_security_init(&cfg));
  TEST_END("test_init_invalid_iocap");
}

static void test_init_close_roundtrip(void)
{
  TEST_BEGIN("test_init_close_roundtrip");
  reset_state();
  const ra_ble_security_config_t cfg = {
    .io_cap           = k_ra_ble_io_cap_no_input_no_out,
    .bonding_enable   = 1U,
    .mitm_required    = 0U,
    .sc_only          = 0U,
    .use_rsip_offload = 0U,
  };
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_security_init(&cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_security_close());
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_ble_security_close());
  TEST_END("test_init_close_roundtrip");
}

static void test_passkey_reply_range(void)
{
  TEST_BEGIN("test_passkey_reply_range");
  reset_state();
  const ra_ble_security_config_t cfg = {.io_cap = k_ra_ble_io_cap_keyboard_only};
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_security_init(&cfg));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ble_security_passkey_reply(0x40U, 1000000U, 1U));
  TEST_END("test_passkey_reply_range");
}

static void test_bond_count(void)
{
  TEST_BEGIN("test_bond_count");
  reset_state();
  uint8_t                        c   = 0xFFU;
  const ra_ble_security_config_t cfg = {.io_cap = k_ra_ble_io_cap_no_input_no_out};
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_ble_security_bond_count(&c));
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_security_init(&cfg));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_ble_security_bond_count(NULL));
  ra_ble_security_test_set_bond_count(2U);
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_security_bond_count(&c));
  TEST_ASSERT_EQ(2U, c);
  TEST_END("test_bond_count");
}

static void test_event_callback(void)
{
  TEST_BEGIN("test_event_callback");
  reset_state();
  const ra_ble_security_config_t cfg = {.io_cap = k_ra_ble_io_cap_display_only};
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_security_init(&cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_security_attach_event_handler(capture_evt, NULL));
  ra_ble_security_event_t evt = {.kind = k_ra_ble_sec_evt_passkey_display, .passkey = 123456U};
  ra_ble_security_test_emit_event(&evt);
  TEST_ASSERT_EQ(1U, s_evt_count);
  TEST_ASSERT_EQ(123456U, s_last.passkey);
  TEST_END("test_event_callback");
}

int main(void)
{
  test_init_null();
  test_init_invalid_iocap();
  test_init_close_roundtrip();
  test_passkey_reply_range();
  test_bond_count();
  test_event_callback();
  return 0;
}
