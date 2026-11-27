/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_ble_hci/src/c6_ble_core.c
 * @brief Hardware-free BLE HCI bring-up orchestration over `ble_hci_c6link`.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.2.0
 */

#include <stdint.h>
#include <string.h>

#include "ble_hci_c6link.h"
#include "c6_ble_hci.h"
#include "ra8_ble.h"
#include "ra8_c6link.h"
#include "ra8_err.h"

typedef enum : uint8_t {
  k_c6_ble_cmd_complete_min  = 4U,  /**< Num_HCI_Cmd + Opcode(2) + Status(1).    */
  k_c6_ble_adv_params_bytes  = 15U, /**< Core 5.3 LE_Set_Advertising_Parameters. */
  k_c6_ble_adv_all_channels  = 0x07U,
  k_c6_ble_status_not_issued = 0xFFU,
} c6_ble_proto_const_t;

typedef struct c6_ble_evt_wait {
  uint16_t expected_opcode;
  bool     done;
  uint8_t  status;
  uint32_t events_seen;
  uint8_t  ret_len;
  uint8_t  ret_params[k_c6_ble_evt_cap_bytes];
} c6_ble_evt_wait_t;

static void
c6_ble_on_hci_event(void* ctx, uint8_t evt_code, const uint8_t* params, uint8_t params_len)
{
  c6_ble_evt_wait_t* const wait = (c6_ble_evt_wait_t*)ctx;
  if (wait == nullptr) {
    return;
  }
  wait->events_seen++;
  if ((evt_code != (uint8_t)k_c6_ble_evt_cmd_complete) || (params == nullptr) ||
      (params_len < (uint8_t)k_c6_ble_cmd_complete_min)) {
    return;
  }
  const uint16_t opcode = (uint16_t)params[1] | ((uint16_t)params[2] << 8U);
  if (opcode != wait->expected_opcode) {
    return;
  }
  wait->status        = params[3];
  const uint8_t trail = (uint8_t)(params_len - (uint8_t)k_c6_ble_cmd_complete_min);
  wait->ret_len =
    (trail <= (uint8_t)k_c6_ble_evt_cap_bytes) ? trail : (uint8_t)k_c6_ble_evt_cap_bytes;
  if (wait->ret_len > 0U) {
    memcpy(wait->ret_params, &params[k_c6_ble_cmd_complete_min], wait->ret_len);
  }
  wait->done = true;
}

static bool c6_ble_await_cmd(const c6_ble_run_cfg_t* cfg,
                             c6_ble_evt_wait_t*      wait,
                             uint16_t                opcode,
                             ra8_err_t               send_err,
                             c6_ble_result_t*        out)
{
  if (send_err != k_ra8_ok) {
    out->cmd_err = send_err;
    return false;
  }
  wait->expected_opcode = opcode;
  wait->done            = false;
  wait->status          = (uint8_t)k_c6_ble_status_not_issued;
  wait->ret_len         = 0U;

  const uint16_t budget =
    (cfg->poll_budget > 0U) ? cfg->poll_budget : (uint16_t)k_c6_ble_cmd_poll_budget;
  for (uint16_t i = 0U; i < budget; i++) {
    const ra8_err_t poll_err = ra8_ble_dispatch();
    if (poll_err != k_ra8_ok) {
      out->cmd_err = poll_err;
      return false;
    }
    if (wait->done) {
      out->hci_status = wait->status;
      if (wait->status != 0x00U) {
        out->cmd_err = k_ra8_err_protocol_error;
        return false;
      }
      out->cmd_err = k_ra8_ok;
      return true;
    }
    if (cfg->wait_ms != nullptr) {
      cfg->wait_ms(cfg->wait_ctx, (uint32_t)k_c6_ble_cmd_poll_ms);
    }
  }
  out->cmd_err = k_ra8_err_timeout;
  return false;
}

static bool c6_ble_setup_link_and_ble(const c6_ble_run_cfg_t* cfg,
                                      c6_ble_evt_wait_t*      wait,
                                      c6_ble_result_t*        out)
{
  const ra8_c6link_cfg_t lcfg = {
    .transport   = cfg->transport,
    .arena       = cfg->arena,
    .arena_bytes = cfg->arena_bytes,
    .event_cb    = nullptr,
    .rx_cb       = nullptr,
    .cb_ctx      = nullptr,
  };
  out->open_err = ra8_c6link_open(cfg->link, &lcfg);
  if (out->open_err != k_ra8_ok) {
    return false;
  }
  out->link_opened = true;

  out->ready_err =
    ra8_c6link_await_ready(cfg->link, (uint16_t)k_ra8_c6link_announce_transfers, &out->fw);
  if (out->ready_err != k_ra8_ok) {
    return false;
  }
  out->ready_ok = true;

  const ra8_ble_config_t bcfg = {.use_external_osc = 0U, .deep_sleep_enable = 0U};
  out->cmd_err                = ra8_ble_open(&bcfg);
  if (out->cmd_err != k_ra8_ok) {
    return false;
  }
  out->ble_opened = true;

  out->cmd_err = ra8_ble_attach_event_handler(c6_ble_on_hci_event, wait);
  if (out->cmd_err != k_ra8_ok) {
    return false;
  }
  out->cmd_err = ble_hci_c6link_init(cfg->link);
  if (out->cmd_err != k_ra8_ok) {
    return false;
  }
  out->hci_bound = true;
  return true;
}

static bool
c6_ble_run_adv_sequence(const c6_ble_run_cfg_t* cfg, c6_ble_evt_wait_t* wait, c6_ble_result_t* out)
{
  const uint8_t int_lo = (uint8_t)((uint16_t)k_c6_ble_adv_interval_slot & 0xFFU);
  const uint8_t int_hi = (uint8_t)((uint16_t)k_c6_ble_adv_interval_slot >> 8U);
  const uint8_t adv_params[k_c6_ble_adv_params_bytes] = {
    int_lo,
    int_hi,
    int_lo,
    int_hi,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    (uint8_t)k_c6_ble_adv_all_channels,
    0x00U,
  };
  const ra8_err_t p_err = ra8_ble_hci_send_command((uint16_t)k_c6_ble_op_le_adv_params,
                                                   adv_params,
                                                   (uint8_t)sizeof(adv_params));
  if (!c6_ble_await_cmd(cfg, wait, (uint16_t)k_c6_ble_op_le_adv_params, p_err, out)) {
    return false;
  }
  out->adv_params_ok = true;

  static const uint8_t k_adv_data[] = {
    0x02U,
    0x01U,
    0x06U,
    0x07U,
    0x09U,
    'R',
    'A',
    '8',
    '-',
    'C',
    '6',
  };
  const ra8_err_t d_err = ra8_ble_set_advertising_data(k_adv_data, (uint8_t)sizeof(k_adv_data));
  if (!c6_ble_await_cmd(cfg, wait, (uint16_t)k_c6_ble_op_le_adv_data, d_err, out)) {
    return false;
  }
  out->adv_data_ok = true;

  const ra8_err_t e_err = ra8_ble_set_advertising_enable(1U);
  if (!c6_ble_await_cmd(cfg, wait, (uint16_t)k_c6_ble_op_le_adv_enable, e_err, out)) {
    return false;
  }
  out->adv_enable_ok = true;
  return true;
}

bool c6_ble_hci_run(const c6_ble_run_cfg_t* cfg, c6_ble_result_t* out)
{
  if (out == nullptr) {
    return false;
  }
  *out            = (c6_ble_result_t){};
  out->open_err   = k_ra8_err_not_initialized;
  out->ready_err  = k_ra8_err_not_initialized;
  out->cmd_err    = k_ra8_err_not_initialized;
  out->hci_status = (uint8_t)k_c6_ble_status_not_issued;
  if ((cfg == nullptr) || (cfg->link == nullptr) || (cfg->arena == nullptr)) {
    return false;
  }

  c6_ble_evt_wait_t wait = {};
  if (!c6_ble_setup_link_and_ble(cfg, &wait, out)) {
    goto cleanup;
  }

  const ra8_err_t rst_err = ra8_ble_hci_send_command((uint16_t)k_c6_ble_op_reset, nullptr, 0U);
  if (!c6_ble_await_cmd(cfg, &wait, (uint16_t)k_c6_ble_op_reset, rst_err, out)) {
    goto cleanup;
  }
  out->reset_ok = true;

  const ra8_err_t addr_err =
    ra8_ble_hci_send_command((uint16_t)k_c6_ble_op_read_bd_addr, nullptr, 0U);
  if (!c6_ble_await_cmd(cfg, &wait, (uint16_t)k_c6_ble_op_read_bd_addr, addr_err, out) ||
      (wait.ret_len < (uint8_t)k_ra8_ble_addr_bytes)) {
    if (out->cmd_err == k_ra8_ok) {
      out->cmd_err = k_ra8_err_protocol_error;
    }
    goto cleanup;
  }
  memcpy(out->bd_addr, wait.ret_params, (size_t)k_ra8_ble_addr_bytes);
  out->bd_addr_ok = true;

  if (c6_ble_run_adv_sequence(cfg, &wait, out)) {
    out->passed = true;
  }

cleanup:
  out->events_seen = wait.events_seen;
  if (out->adv_enable_ok) {
    (void)ra8_ble_set_advertising_enable(0U);
  }
  if (out->hci_bound) {
    (void)ble_hci_c6link_deinit();
  }
  if (out->ble_opened) {
    (void)ra8_ble_attach_event_handler(nullptr, nullptr);
    (void)ra8_ble_close();
  }
  if (out->link_opened) {
    (void)ra8_c6link_close(cfg->link);
  }
  return out->passed;
}
