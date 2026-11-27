/**
 * @file tests/wireless/src/test_app_c6_ble_nimble.c
 * @brief Host integration tests driving real Apache NimBLE over `ra8_ble`,
 *        `ble_hci_c6link`, and `ra8_c6link` against `ra8_c6_model`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ble_hci_c6link.h"
#include "ble_hci_ra8_ble.h"
#include "c6_ble_nimble.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nimble_port_alloc.h"
#include "ra8_attributes.h"
#include "ra8_ble.h"
#include "ra8_ble_internal.h"
#include "ra8_c6_model.h"
#include "ra8_c6link.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_t_arena_bytes      = 4096U,
  k_t_max_opcodes      = 24U,
  k_t_max_evts         = 16U,
  k_t_adv_buf_bytes    = 32U,
  k_t_h4_cmd           = 0x01U,
  k_t_h4_acl           = 0x02U,
  k_t_h4_evt           = 0x04U,
  k_t_evt_disconn_comp = 0x05U,
  k_t_evt_cmd_complete = 0x0EU,
  k_t_evt_le_meta      = 0x3EU,
  k_t_le_sub_conn_comp = 0x01U,
  k_t_conn_handle      = 0x0001U,
  k_t_pump_budget      = 64U,
} test_nimble_const_t;

typedef struct test_c6_responder {
  ra8_err_t (*base_transfer)(void* ctx, const uint8_t* tx, uint8_t* rx, uint16_t len);
  uint16_t seen_opcodes[k_t_max_opcodes];
  uint8_t  seen_n;
  uint8_t  adv_len;
  uint8_t  adv_data[k_t_adv_buf_bytes];
  bool     adv_enabled;
  bool     acl_tx_seen;
  uint8_t  acl_tx_att_op;
  uint16_t acl_tx_len;
} test_c6_responder_t;

static test_c6_responder_t s_rsp;
static uint8_t             s_arena[k_t_arena_bytes];
static c6_ble_nimble_evt_t s_evt_log[k_t_max_evts];
static uint8_t             s_evt_n;

static const uint8_t k_c6_bd_addr[k_ra8_ble_addr_bytes] = {
  0xCEU,
  0x4EU,
  0x0CU,
  0xE6U,
  0xEBU,
  0xACU,
};

static const uint8_t k_star_hci0_addr[k_ra8_ble_addr_bytes] = {
  0x8CU,
  0x04U,
  0x4DU,
  0x67U,
  0xCFU,
  0x2CU,
};

RA8_INTERNAL static void
test_emit_cc_with_payload(uint16_t opcode, const uint8_t* extra, uint8_t extra_len)
{
  uint8_t evt[72] = {};
  evt[0]          = (uint8_t)k_t_h4_evt;
  evt[1]          = (uint8_t)k_t_evt_cmd_complete;
  evt[2]          = (uint8_t)(4U + extra_len);
  evt[3]          = 0x01U;
  evt[4]          = (uint8_t)(opcode & 0xFFU);
  evt[5]          = (uint8_t)(opcode >> 8U);
  evt[6]          = 0x00U;
  if ((extra != nullptr) && (extra_len > 0U)) {
    (void)memcpy(&evt[7], extra, extra_len);
  }
  ra8_c6_model_emit_hci(evt, (uint16_t)(7U + extra_len));
}

RA8_INTERNAL static void test_handle_hci_cmd(uint16_t opcode, const uint8_t* hci_tx)
{
  if (s_rsp.seen_n < (uint8_t)k_t_max_opcodes) {
    s_rsp.seen_opcodes[s_rsp.seen_n] = opcode;
    s_rsp.seen_n++;
  }

  switch (opcode) {
    case 0x1001U: {
      const uint8_t ver[8] = {11U, 0x00U, 0x00U, 11U, 0xE5U, 0x02U, 0x00U, 0x00U};
      test_emit_cc_with_payload(opcode, ver, (uint8_t)sizeof(ver));
      break;
    }
    case 0x1002U: {
      const uint8_t cmds[64] = {};
      test_emit_cc_with_payload(opcode, cmds, (uint8_t)sizeof(cmds));
      break;
    }
    case 0x1003U: {
      uint8_t feat[8] = {};
      feat[4]         = 0x60U; /* LE Supported (Controller) */
      test_emit_cc_with_payload(opcode, feat, (uint8_t)sizeof(feat));
      break;
    }
    case 0x2002U: {
      const uint8_t le_buf[3] = {0xFBU, 0x00U, 0x04U};
      test_emit_cc_with_payload(opcode, le_buf, (uint8_t)sizeof(le_buf));
      break;
    }
    case 0x2003U: {
      const uint8_t le_feat[8] = {};
      test_emit_cc_with_payload(opcode, le_feat, (uint8_t)sizeof(le_feat));
      break;
    }
    case 0x1009U:
      test_emit_cc_with_payload(opcode, k_c6_bd_addr, (uint8_t)sizeof(k_c6_bd_addr));
      break;
    case 0x2008U:
      s_rsp.adv_len = hci_tx[3];
      if (s_rsp.adv_len <= 31U) {
        (void)memcpy(s_rsp.adv_data, &hci_tx[4], s_rsp.adv_len);
      }
      test_emit_cc_with_payload(opcode, nullptr, 0U);
      break;
    case 0x200AU:
      s_rsp.adv_enabled = (hci_tx[3] != 0U);
      test_emit_cc_with_payload(opcode, nullptr, 0U);
      break;
    default:
      test_emit_cc_with_payload(opcode, nullptr, 0U);
      break;
  }
}

RA8_INTERNAL static ra8_err_t
test_model_transfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint16_t len)
{
  ra8_c6_model_t* const model = ra8_c6_model();
  model->hci_tx_len           = 0U;
  model->hci_tx_pkt_type      = 0U;

  const ra8_err_t err = s_rsp.base_transfer(ctx, tx, rx, len);
  if (err != k_ra8_ok) {
    return err;
  }

  if ((model->hci_tx_pkt_type == (uint8_t)k_t_h4_cmd) && (model->hci_tx_len >= 3U)) {
    const uint16_t opcode = (uint16_t)model->hci_tx[0] | ((uint16_t)model->hci_tx[1] << 8U);
    test_handle_hci_cmd(opcode, model->hci_tx);
  } else if ((model->hci_tx_pkt_type == (uint8_t)k_t_h4_acl) && (model->hci_tx_len >= 9U)) {
    s_rsp.acl_tx_seen   = true;
    s_rsp.acl_tx_len    = model->hci_tx_len;
    s_rsp.acl_tx_att_op = model->hci_tx[8];
  }
  return err;
}

RA8_INTERNAL static void test_pump_once(void* ctx)
{
  (void)ctx;
  TEST_ASSERT_EQ(k_ra8_ok, ble_hci_ra8_ble_dispatch());
}

RA8_INTERNAL static void test_drain_host_events(void)
{
  struct ble_npl_eventq* const evq = nimble_port_get_dflt_eventq();
  for (uint32_t i = 0U; i < (uint32_t)k_t_pump_budget; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ble_hci_ra8_ble_dispatch());
    struct ble_npl_event* const ev = ble_npl_eventq_get(evq, 0U);
    if (ev != nullptr) {
      ble_npl_event_run(ev);
    } else if (ble_npl_eventq_is_empty(evq)) {
      break;
    }
  }
}

RA8_INTERNAL static void
test_record_evt(void* ctx, c6_ble_nimble_evt_t evt, const c6_ble_nimble_state_t* st)
{
  (void)ctx;
  (void)st;
  if (s_evt_n < (uint8_t)k_t_max_evts) {
    s_evt_log[s_evt_n] = evt;
    s_evt_n++;
  }
}

/**
 * @par MC/DC:
 * (drives real NimBLE host startup, GAP advertising, LE Connection Complete,
 * ATT MTU exchange, Disconnect, and automatic advertising restart over C6 model)
 */
RA8_INTERNAL static void test_c6_ble_nimble_full_peripheral_lifecycle(void)
{
  TEST_BEGIN("c6_ble_nimble full GAP/GATT lifecycle over ra8_c6_model");
  ra8_fake_mmap_reset();
  nimble_npl_host_reset();
  c6_ble_nimble_reset_state();
  (void)memset(s_evt_log, 0, sizeof(s_evt_log));
  s_evt_n = 0U;

  ra8_c6link_transport_t transport = {};
  ra8_c6_model_reset();
  ra8_c6_model_bind(&transport);
  s_rsp               = (test_c6_responder_t){};
  s_rsp.base_transfer = transport.transfer;
  transport.transfer  = test_model_transfer;

  ra8_c6link_t             link = {};
  c6_ble_nimble_link_cfg_t lcfg = {
    .link        = &link,
    .transport   = transport,
    .arena       = s_arena,
    .arena_bytes = (uint32_t)sizeof(s_arena),
  };
  TEST_ASSERT(c6_ble_nimble_bind_link(&lcfg));

  const c6_ble_nimble_app_cfg_t acfg = {
    .device_name         = "EK-RA8D2",
    .initial_battery_pct = (uint8_t)k_c6_ble_nimble_battery_init,
    .notify_cb           = test_record_evt,
    .notify_ctx          = nullptr,
  };
  TEST_ASSERT(c6_ble_nimble_stack_init(&acfg));

  nimble_npl_host_set_pump(test_pump_once, nullptr);
  test_drain_host_events();

  c6_ble_nimble_state_t st = {};
  c6_ble_nimble_get_state(&st);
  TEST_ASSERT(st.synced);
  TEST_ASSERT(st.advertising);
  TEST_ASSERT(s_rsp.adv_enabled);
  TEST_ASSERT_EQ(1U, st.adv_starts);
  TEST_ASSERT_EQ(0, memcmp(st.bd_addr, k_c6_bd_addr, sizeof(k_c6_bd_addr)));

  /* Adv payload carries Flags + Complete Name "EK-RA8D2" + Battery UUID 0x180F (17B). */
  TEST_ASSERT_EQ(17U, s_rsp.adv_len);
  const uint8_t expected_name[10] = {0x09U, 0x09U, 'E', 'K', '-', 'R', 'A', '8', 'D', '2'};
  const uint8_t expected_bas[4]   = {0x03U, 0x03U, 0x0FU, 0x18U};
  bool          saw_name          = false;
  bool          saw_bas           = false;
  for (uint8_t i = 0U; (uint8_t)(i + sizeof(expected_name)) <= s_rsp.adv_len; i++) {
    if (memcmp(&s_rsp.adv_data[i], expected_name, sizeof(expected_name)) == 0) {
      saw_name = true;
    }
  }
  for (uint8_t i = 0U; (uint8_t)(i + sizeof(expected_bas)) <= s_rsp.adv_len; i++) {
    if (memcmp(&s_rsp.adv_data[i], expected_bas, sizeof(expected_bas)) == 0) {
      saw_bas = true;
    }
  }
  TEST_ASSERT(saw_name);
  TEST_ASSERT(saw_bas);

  /* Deliver synthetic LE Connection Complete from C6 controller. */
  const uint8_t conn_evt[22] = {
    (uint8_t)k_t_h4_evt,
    (uint8_t)k_t_evt_le_meta,
    19U,
    (uint8_t)k_t_le_sub_conn_comp,
    0x00U,
    (uint8_t)(k_t_conn_handle & 0xFFU),
    (uint8_t)(k_t_conn_handle >> 8U),
    0x01U,
    0x00U,
    0x8CU,
    0x04U,
    0x4DU,
    0x67U,
    0xCFU,
    0x2CU,
    0x18U,
    0x00U,
    0x00U,
    0x00U,
    0xC8U,
    0x00U,
    0x00U,
  };
  ra8_c6_model_emit_hci(conn_evt, (uint16_t)sizeof(conn_evt));
  test_drain_host_events();

  c6_ble_nimble_get_state(&st);
  TEST_ASSERT(st.connected);
  TEST_ASSERT(!st.advertising);
  TEST_ASSERT_EQ((uint16_t)k_t_conn_handle, st.conn_handle);
  TEST_ASSERT_EQ(1U, st.connect_count);
  TEST_ASSERT_EQ(0, memcmp(st.peer_addr, k_star_hci0_addr, sizeof(k_star_hci0_addr)));

  /* Deliver ATT Exchange MTU Request over ACL -> expect ATT MTU Response back. */
  const uint8_t acl_att_mtu[12] = {
    (uint8_t)k_t_h4_acl,
    0x01U,
    0x20U,
    0x07U,
    0x00U,
    0x03U,
    0x00U,
    0x04U,
    0x00U,
    0x02U,
    0xF7U,
    0x00U,
  };
  ra8_c6_model_emit_hci(acl_att_mtu, (uint16_t)sizeof(acl_att_mtu));
  test_drain_host_events();
  TEST_ASSERT(s_rsp.acl_tx_seen);
  TEST_ASSERT_EQ(0x03U, s_rsp.acl_tx_att_op);

  /* Deliver Disconnection Complete -> verify reconnect advertising restarts. */
  const uint8_t disconn_evt[7] = {
    (uint8_t)k_t_h4_evt,
    (uint8_t)k_t_evt_disconn_comp,
    0x04U,
    0x00U,
    (uint8_t)(k_t_conn_handle & 0xFFU),
    (uint8_t)(k_t_conn_handle >> 8U),
    0x13U,
  };
  ra8_c6_model_emit_hci(disconn_evt, (uint16_t)sizeof(disconn_evt));
  test_drain_host_events();

  c6_ble_nimble_get_state(&st);
  TEST_ASSERT(!st.connected);
  TEST_ASSERT(st.advertising);
  TEST_ASSERT_EQ(1U, st.disconnect_count);
  TEST_ASSERT_EQ(2U, st.adv_starts);

  c6_ble_nimble_reset_state();
  (void)ra8_c6link_close(&link);
  TEST_END("c6_ble_nimble full GAP/GATT lifecycle over ra8_c6_model");
}

/**
 * @par MC/DC:
 * (verifies null argument guards, mute-link failure, and that teardown leaves
 * ra8_ble unbound without restoring loopback)
 */
RA8_INTERNAL static void test_c6_ble_nimble_guards_and_unbind(void)
{
  TEST_BEGIN("c6_ble_nimble guards and unbound post-reset contract");
  ra8_fake_mmap_reset();
  nimble_npl_host_reset();
  c6_ble_nimble_reset_state();

  TEST_ASSERT(!c6_ble_nimble_bind_link(nullptr));
  TEST_ASSERT(!c6_ble_nimble_stack_init(nullptr));

  ra8_c6link_transport_t transport = {};
  ra8_c6_model_reset();
  ra8_c6_model_bind(&transport);
  ra8_c6_model()->handshake = false;

  ra8_c6link_t             link = {};
  c6_ble_nimble_link_cfg_t lcfg = {
    .link        = &link,
    .transport   = transport,
    .arena       = s_arena,
    .arena_bytes = (uint32_t)sizeof(s_arena),
  };
  TEST_ASSERT(!c6_ble_nimble_bind_link(&lcfg));

  /* Unbound state after reset refuses ra8_ble operations cleanly. */
  const ra8_ble_config_t bcfg = {.use_external_osc = 0U, .deep_sleep_enable = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_open(&bcfg));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ble_dispatch());
  uint16_t cap_len = 99U;
  (void)ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT_EQ(0U, cap_len);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_close());
  TEST_END("c6_ble_nimble guards and unbound post-reset contract");
}

/**
 * @par MC/DC:
 * (exercises static NimBLE SRAM allocator alloc/grow/shrink/coalesce/oom bounds)
 */
RA8_INTERNAL static void test_nimble_port_static_alloc(void)
{
  TEST_BEGIN("nimble_port_alloc static SRAM coalescing pool");
  uint8_t* p1 = (uint8_t*)nimble_port_malloc(64U);
  uint8_t* p2 = (uint8_t*)nimble_port_malloc(64U);
  TEST_ASSERT(p1 != nullptr);
  TEST_ASSERT(p2 != nullptr);
  (void)memset(p1, 0xA5, 64U);
  nimble_port_free(p2);

  uint8_t* p1_grown = (uint8_t*)nimble_port_realloc(p1, 128U);
  TEST_ASSERT_EQ(p1, p1_grown);
  TEST_ASSERT_EQ(0xA5U, p1_grown[63]);

  TEST_ASSERT(nimble_port_malloc(65536U) == nullptr);
  nimble_port_free(p1_grown);
  TEST_ASSERT(nimble_port_realloc(nimble_port_malloc(16U), 0U) == nullptr);
  TEST_END("nimble_port_alloc static SRAM coalescing pool");
}

int main(void)
{
  test_c6_ble_nimble_full_peripheral_lifecycle();
  test_c6_ble_nimble_guards_and_unbind();
  test_nimble_port_static_alloc();
  return 0;
}
