/**
 * @file test_app_c6_ble_hci.c
 * @brief Host integration tests for `examples/ek_ra8d2/hw_pending/c6/c6_ble_hci`.
 * @details Drives `c6_ble_hci_run()` end-to-end through `ble_hci_c6link`,
 *          `ra8_ble`, and `ra8_c6link` against the deterministic `ra8_c6_model`
 *          co-processor, verifying happy-path HCI bring-up, controller status
 *          failures, Event timeouts, and null/resource-guard rejection.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ble_hci_c6link.h"
#include "c6_ble_hci.h"
#include "ra8_attributes.h"
#include "ra8_ble.h"
#include "ra8_c6_model.h"
#include "ra8_c6link.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_arena_bytes        = 4096U,
  k_test_poll_budget        = 16U,
  k_test_max_seen_opcodes   = 12U,
  k_test_hci_evt_hdr        = 4U,
  k_test_hci_cc_base        = 4U,
  k_test_hci_cc_bd_addr     = 10U,
  k_test_hci_cmd_disallowed = 0x0CU,
  k_test_h4_pkt_cmd         = 0x01U,
  k_test_h4_pkt_evt         = 0x04U,
} test_c6_ble_const_t;

typedef struct test_hci_script {
  ra8_err_t (*base_transfer)(void* ctx, const uint8_t* tx, uint8_t* rx, uint16_t len);
  bool     reply_enabled;
  uint16_t fail_opcode;
  uint8_t  fail_status;
  uint16_t drop_opcode;
  uint16_t seen_opcodes[k_test_max_seen_opcodes];
  uint8_t  seen_enables[k_test_max_seen_opcodes];
  uint8_t  seen_n;
  uint32_t wait_calls;
} test_hci_script_t;

static test_hci_script_t s_script = {};
static uint8_t           s_arena[k_test_arena_bytes];
static const uint8_t     k_expected_bd_addr[k_ra8_ble_addr_bytes] = {
  0x11U,
  0x22U,
  0x33U,
  0x44U,
  0x55U,
  0x66U,
};

static void test_emit_cmd_complete(uint16_t opcode)
{
  uint8_t       evt[k_test_hci_evt_hdr + k_test_hci_cc_bd_addr] = {};
  const uint8_t status    = (opcode == s_script.fail_opcode) ? s_script.fail_status : 0x00U;
  const uint8_t param_len = (opcode == (uint16_t)k_c6_ble_op_read_bd_addr)
                              ? (uint8_t)k_test_hci_cc_bd_addr
                              : (uint8_t)k_test_hci_cc_base;

  evt[0] = (uint8_t)k_test_h4_pkt_evt;
  evt[1] = (uint8_t)k_c6_ble_evt_cmd_complete;
  evt[2] = param_len;
  evt[3] = 0x01U;
  evt[4] = (uint8_t)(opcode & 0xFFU);
  evt[5] = (uint8_t)(opcode >> 8U);
  evt[6] = status;
  if (opcode == (uint16_t)k_c6_ble_op_read_bd_addr) {
    memcpy(&evt[7], k_expected_bd_addr, sizeof(k_expected_bd_addr));
  }
  ra8_c6_model_emit_hci(evt, (uint16_t)(3U + (uint16_t)param_len));
}

static ra8_err_t test_intercept_transfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint16_t len)
{
  ra8_c6_model_t* const model = ra8_c6_model();
  model->hci_tx_len           = 0U;

  const ra8_err_t err = s_script.base_transfer(ctx, tx, rx, len);
  if ((err != k_ra8_ok) || (model->hci_tx_pkt_type != (uint8_t)k_test_h4_pkt_cmd) ||
      (model->hci_tx_len < 3U)) {
    return err;
  }

  const uint16_t opcode = (uint16_t)model->hci_tx[0] | ((uint16_t)model->hci_tx[1] << 8U);
  if (s_script.seen_n < (uint8_t)k_test_max_seen_opcodes) {
    s_script.seen_opcodes[s_script.seen_n] = opcode;
    s_script.seen_enables[s_script.seen_n] =
      (opcode == (uint16_t)k_c6_ble_op_le_adv_enable) ? model->hci_tx[3] : 0U;
    s_script.seen_n++;
  }

  if (s_script.reply_enabled && (opcode != s_script.drop_opcode)) {
    test_emit_cmd_complete(opcode);
  }
  return err;
}

static void test_wait_cb(void* ctx, uint32_t ms)
{
  (void)ctx;
  (void)ms;
  s_script.wait_calls++;
}

static c6_ble_run_cfg_t test_make_cfg(ra8_c6link_t* link)
{
  ra8_c6link_transport_t transport = {};
  ra8_c6_model_reset();
  ra8_c6_model_bind(&transport);

  s_script               = (test_hci_script_t){};
  s_script.base_transfer = transport.transfer;
  s_script.reply_enabled = true;
  transport.transfer     = test_intercept_transfer;

  return (c6_ble_run_cfg_t){
    .link        = link,
    .transport   = transport,
    .arena       = s_arena,
    .arena_bytes = (uint32_t)sizeof(s_arena),
    .wait_ms     = test_wait_cb,
    .wait_ctx    = nullptr,
    .poll_budget = (uint16_t)k_test_poll_budget,
  };
}

RA8_INTERNAL static void test_c6_ble_happy_path(void)
{
  TEST_BEGIN("c6_ble_hci happy path sends HCI sequence and enables adv");
  ra8_c6link_t     link = {};
  c6_ble_run_cfg_t cfg  = test_make_cfg(&link);
  c6_ble_result_t  res  = {};

  TEST_ASSERT(c6_ble_hci_run(&cfg, &res));
  TEST_ASSERT(res.link_opened);
  TEST_ASSERT(res.ready_ok);
  TEST_ASSERT(res.ble_opened);
  TEST_ASSERT(res.hci_bound);
  TEST_ASSERT(res.reset_ok);
  TEST_ASSERT(res.bd_addr_ok);
  TEST_ASSERT(res.adv_params_ok);
  TEST_ASSERT(res.adv_data_ok);
  TEST_ASSERT(res.adv_enable_ok);
  TEST_ASSERT(res.passed);
  TEST_ASSERT_EQ(k_ra8_ok, res.open_err);
  TEST_ASSERT_EQ(k_ra8_ok, res.ready_err);
  TEST_ASSERT_EQ(k_ra8_ok, res.cmd_err);
  TEST_ASSERT_EQ(0x00U, res.hci_status);
  TEST_ASSERT_EQ(5U, res.events_seen);
  TEST_ASSERT_EQ((uint32_t)k_c6m_fw_major, res.fw.major);
  TEST_ASSERT_EQ(0, memcmp(res.bd_addr, k_expected_bd_addr, sizeof(k_expected_bd_addr)));
  TEST_ASSERT_EQ(6U, s_script.seen_n);
  TEST_ASSERT_EQ((uint16_t)k_c6_ble_op_reset, s_script.seen_opcodes[0]);
  TEST_ASSERT_EQ((uint16_t)k_c6_ble_op_read_bd_addr, s_script.seen_opcodes[1]);
  TEST_ASSERT_EQ((uint16_t)k_c6_ble_op_le_adv_params, s_script.seen_opcodes[2]);
  TEST_ASSERT_EQ((uint16_t)k_c6_ble_op_le_adv_data, s_script.seen_opcodes[3]);
  TEST_ASSERT_EQ((uint16_t)k_c6_ble_op_le_adv_enable, s_script.seen_opcodes[4]);
  TEST_ASSERT_EQ(1U, s_script.seen_enables[4]);
  TEST_ASSERT_EQ((uint16_t)k_c6_ble_op_le_adv_enable, s_script.seen_opcodes[5]);
  TEST_ASSERT_EQ(0U, s_script.seen_enables[5]);
  TEST_ASSERT(!ble_hci_c6link_is_bound());
  TEST_END("c6_ble_hci happy path sends HCI sequence and enables adv");
}

RA8_INTERNAL static void test_c6_ble_status_failure_stops_cleanly(void)
{
  TEST_BEGIN("c6_ble_hci stops cleanly on non-zero HCI Command Complete status");
  ra8_c6link_t     link = {};
  c6_ble_run_cfg_t cfg  = test_make_cfg(&link);
  c6_ble_result_t  res  = {};

  s_script.fail_opcode = (uint16_t)k_c6_ble_op_le_adv_data;
  s_script.fail_status = (uint8_t)k_test_hci_cmd_disallowed;

  TEST_ASSERT(!c6_ble_hci_run(&cfg, &res));
  TEST_ASSERT(res.reset_ok);
  TEST_ASSERT(res.bd_addr_ok);
  TEST_ASSERT(res.adv_params_ok);
  TEST_ASSERT(!res.adv_data_ok);
  TEST_ASSERT(!res.adv_enable_ok);
  TEST_ASSERT(!res.passed);
  TEST_ASSERT_EQ((uint8_t)k_test_hci_cmd_disallowed, res.hci_status);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, res.cmd_err);
  TEST_ASSERT(!ble_hci_c6link_is_bound());
  TEST_END("c6_ble_hci stops cleanly on non-zero HCI Command Complete status");
}

RA8_INTERNAL static void test_c6_ble_missing_event_times_out(void)
{
  TEST_BEGIN("c6_ble_hci reports timeout when HCI Command Complete never arrives");
  ra8_c6link_t     link = {};
  c6_ble_run_cfg_t cfg  = test_make_cfg(&link);
  c6_ble_result_t  res  = {};

  s_script.drop_opcode = (uint16_t)k_c6_ble_op_reset;

  TEST_ASSERT(!c6_ble_hci_run(&cfg, &res));
  TEST_ASSERT(res.hci_bound);
  TEST_ASSERT(!res.reset_ok);
  TEST_ASSERT(!res.passed);
  TEST_ASSERT_EQ(k_ra8_err_timeout, res.cmd_err);
  TEST_ASSERT(s_script.wait_calls >= (uint32_t)k_test_poll_budget);
  TEST_ASSERT(!ble_hci_c6link_is_bound());
  TEST_END("c6_ble_hci reports timeout when HCI Command Complete never arrives");
}

RA8_INTERNAL static void test_c6_ble_input_and_link_guards(void)
{
  TEST_BEGIN("c6_ble_hci rejects null arguments and mute link");
  TEST_ASSERT(!c6_ble_hci_run(nullptr, nullptr));

  c6_ble_result_t res = {};
  TEST_ASSERT(!c6_ble_hci_run(nullptr, &res));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, res.open_err);

  ra8_c6link_t     link    = {};
  c6_ble_run_cfg_t bad_cfg = test_make_cfg(&link);
  bad_cfg.arena            = nullptr;
  TEST_ASSERT(!c6_ble_hci_run(&bad_cfg, &res));

  c6_ble_run_cfg_t mute_cfg = test_make_cfg(&link);
  ra8_c6_model()->mute      = true;
  TEST_ASSERT(!c6_ble_hci_run(&mute_cfg, &res));
  TEST_ASSERT(res.link_opened);
  TEST_ASSERT(!res.ready_ok);
  TEST_ASSERT(!res.ble_opened);
  TEST_END("c6_ble_hci rejects null arguments and mute link");
}

int main(void)
{
  test_c6_ble_happy_path();
  test_c6_ble_status_failure_stops_cleanly();
  test_c6_ble_missing_event_times_out();
  test_c6_ble_input_and_link_guards();
  return 0;
}
