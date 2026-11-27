/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_ble_nimble/src/c6_ble_nimble_core.c
 * @brief Hardware-free Apache NimBLE GAP/GATT peripheral core over `ble_hci_c6link`.
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
#include "ble_hci_ra8_ble.h"
#include "c6_ble_nimble.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "ra8_attributes.h"
#include "ra8_ble.h"
#include "ra8_c6link.h"
#include "ra8_err.h"
#include "services/bas/ble_svc_bas.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

static c6_ble_nimble_state_t     s_state;
static c6_ble_nimble_notify_fn_t s_notify_cb;
static void*                     s_notify_ctx;
static char                      s_dev_name[k_c6_ble_nimble_name_max + 1U] = "EK-RA8D2";
static uint8_t                   s_dev_name_len                            = 8U;

RA8_INTERNAL static void internal_emit_notify(c6_ble_nimble_evt_t evt)
{
  if (s_notify_cb != nullptr) {
    s_notify_cb(s_notify_ctx, evt, &s_state);
  }
}

RA8_INTERNAL static int internal_gap_event_cb(struct ble_gap_event* event, void* arg);

RA8_INTERNAL static void internal_start_advertising(void)
{
  static const ble_uuid16_t k_bas_uuid = BLE_UUID16_INIT((uint16_t)k_c6_ble_nimble_bas_uuid16);
  struct ble_hs_adv_fields  fields     = {};

  fields.flags               = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.name                = (const uint8_t*)s_dev_name;
  fields.name_len            = s_dev_name_len;
  fields.name_is_complete    = 1U;
  fields.uuids16             = &k_bas_uuid;
  fields.num_uuids16         = 1U;
  fields.uuids16_is_complete = 1U;

  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    s_state.advertising = false;
    s_state.last_status = rc;
    internal_emit_notify(k_c6_ble_nimble_evt_adv_fail);
    return;
  }

  struct ble_gap_adv_params adv_params = {};
  adv_params.conn_mode                 = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode                 = BLE_GAP_DISC_MODE_GEN;

  rc = ble_gap_adv_start(s_state.own_addr_type,
                         nullptr,
                         BLE_HS_FOREVER,
                         &adv_params,
                         internal_gap_event_cb,
                         nullptr);
  if (rc != 0) {
    s_state.advertising = false;
    s_state.last_status = rc;
    internal_emit_notify(k_c6_ble_nimble_evt_adv_fail);
    return;
  }

  s_state.advertising = true;
  s_state.last_status = 0;
  s_state.adv_starts++;
  internal_emit_notify(k_c6_ble_nimble_evt_adv_ok);
}

RA8_INTERNAL static void internal_capture_peer_addr(uint16_t conn_handle)
{
  struct ble_gap_conn_desc desc = {};
  if (ble_gap_conn_find(conn_handle, &desc) == 0) {
    (void)memcpy(s_state.peer_addr, desc.peer_id_addr.val, (size_t)k_ra8_ble_addr_bytes);
  }
}

RA8_INTERNAL static int internal_gap_event_cb(struct ble_gap_event* event, void* arg)
{
  (void)arg;
  if (event == nullptr) {
    return 0;
  }

  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      s_state.advertising = false;
      s_state.last_status = event->connect.status;
      if (event->connect.status == 0) {
        s_state.connected   = true;
        s_state.conn_handle = event->connect.conn_handle;
        s_state.connect_count++;
        internal_capture_peer_addr(event->connect.conn_handle);
        internal_emit_notify(k_c6_ble_nimble_evt_connected);
      } else {
        s_state.connected = false;
        internal_emit_notify(k_c6_ble_nimble_evt_conn_fail);
        internal_start_advertising();
      }
      break;

    case BLE_GAP_EVENT_DISCONNECT:
      s_state.connected   = false;
      s_state.advertising = false;
      s_state.conn_handle = event->disconnect.conn.conn_handle;
      s_state.last_status = event->disconnect.reason;
      s_state.disconnect_count++;
      internal_emit_notify(k_c6_ble_nimble_evt_disconn);
      internal_start_advertising();
      break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
      s_state.advertising = false;
      internal_start_advertising();
      break;

    case BLE_GAP_EVENT_SUBSCRIBE:
      s_state.subscribe_attr_handle = event->subscribe.attr_handle;
      s_state.subscribe_count++;
      internal_emit_notify(k_c6_ble_nimble_evt_subscribe);
      break;

    case BLE_GAP_EVENT_MTU:
      s_state.conn_handle = event->mtu.conn_handle;
      s_state.mtu         = event->mtu.value;
      internal_emit_notify(k_c6_ble_nimble_evt_mtu);
      break;

    default:
      break;
  }
  return 0;
}

RA8_INTERNAL static void internal_on_sync(void)
{
  int rc = ble_hs_util_ensure_addr(0);
  if (rc != 0) {
    s_state.synced      = false;
    s_state.last_status = rc;
    internal_emit_notify(k_c6_ble_nimble_evt_adv_fail);
    return;
  }

  rc = ble_hs_id_infer_auto(0, &s_state.own_addr_type);
  if (rc != 0) {
    s_state.synced      = false;
    s_state.last_status = rc;
    internal_emit_notify(k_c6_ble_nimble_evt_adv_fail);
    return;
  }

  (void)ble_hs_id_copy_addr(s_state.own_addr_type, s_state.bd_addr, nullptr);
  s_state.synced      = true;
  s_state.last_status = 0;
  s_state.sync_count++;
  internal_emit_notify(k_c6_ble_nimble_evt_sync_ok);

  internal_start_advertising();
}

RA8_INTERNAL static void internal_on_reset(int reason)
{
  s_state.synced       = false;
  s_state.advertising  = false;
  s_state.connected    = false;
  s_state.reset_reason = reason;
  internal_emit_notify(k_c6_ble_nimble_evt_host_reset);
}

RA8_INTERNAL static void internal_set_device_name(const char* name)
{
  const char* src = (name != nullptr && name[0] != '\0') ? name : "EK-RA8D2";
  uint8_t     len = 0U;
  while ((len < (uint8_t)k_c6_ble_nimble_name_max) && (src[len] != '\0')) {
    s_dev_name[len] = src[len];
    len++;
  }
  s_dev_name[len] = '\0';
  s_dev_name_len  = len;
}

bool c6_ble_nimble_bind_link(const c6_ble_nimble_link_cfg_t* cfg)
{
  s_state.open_err  = k_ra8_err_not_initialized;
  s_state.ready_err = k_ra8_err_not_initialized;
  s_state.ble_err   = k_ra8_err_not_initialized;
  if ((cfg == nullptr) || (cfg->link == nullptr) || (cfg->arena == nullptr)) {
    return false;
  }

  const ra8_c6link_cfg_t lcfg = {
    .transport   = cfg->transport,
    .arena       = cfg->arena,
    .arena_bytes = cfg->arena_bytes,
    .event_cb    = nullptr,
    .rx_cb       = nullptr,
    .cb_ctx      = nullptr,
  };
  s_state.open_err = ra8_c6link_open(cfg->link, &lcfg);
  if (s_state.open_err != k_ra8_ok) {
    return false;
  }
  s_state.link_opened = true;

  s_state.ready_err =
    ra8_c6link_await_ready(cfg->link, (uint16_t)k_ra8_c6link_announce_transfers, &s_state.fw);
  if (s_state.ready_err != k_ra8_ok) {
    return false;
  }
  s_state.ready_ok = true;

  const ra8_ble_config_t bcfg = {.use_external_osc = 0U, .deep_sleep_enable = 0U};
  s_state.ble_err             = ra8_ble_open(&bcfg);
  if (s_state.ble_err != k_ra8_ok) {
    return false;
  }
  s_state.ble_opened = true;

  s_state.ble_err = ble_hci_c6link_init(cfg->link);
  if (s_state.ble_err != k_ra8_ok) {
    return false;
  }
  s_state.c6_bound = true;

  s_state.ble_err = ble_hci_ra8_ble_init();
  if (s_state.ble_err != k_ra8_ok) {
    return false;
  }
  s_state.hs_bound = true;
  return true;
}

bool c6_ble_nimble_stack_init(const c6_ble_nimble_app_cfg_t* cfg)
{
  if (cfg == nullptr) {
    return false;
  }
  s_notify_cb           = cfg->notify_cb;
  s_notify_ctx          = cfg->notify_ctx;
  s_state.battery_level = cfg->initial_battery_pct;
  internal_set_device_name(cfg->device_name);

  ble_hs_cfg.reset_cb        = internal_on_reset;
  ble_hs_cfg.sync_cb         = internal_on_sync;
  ble_hs_cfg.store_read_cb   = ble_store_config_read;
  ble_hs_cfg.store_write_cb  = ble_store_config_write;
  ble_hs_cfg.store_delete_cb = ble_store_config_delete;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

  nimble_port_init();
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_svc_bas_init();

  if (ble_svc_gap_device_name_set(s_dev_name) != 0) {
    return false;
  }
  if (ble_svc_bas_battery_level_set(s_state.battery_level) != 0) {
    return false;
  }
  return true;
}

void c6_ble_nimble_get_state(c6_ble_nimble_state_t* out)
{
  if (out != nullptr) {
    *out = s_state;
  }
}

void c6_ble_nimble_reset_state(void)
{
  (void)ble_hci_ra8_ble_deinit();
  (void)ble_hci_c6link_deinit();
  if (s_state.ble_opened) {
    (void)ra8_ble_close();
  }
  s_notify_cb  = nullptr;
  s_notify_ctx = nullptr;
  s_state      = (c6_ble_nimble_state_t){};
  internal_set_device_name("EK-RA8D2");
}
