/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_ble_nimble/src/main.c
 * @brief ThreadX runner for Apache NimBLE GATT peripheral over ESP32-C6 SPI HCI.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include "ble_hci_ra8_ble.h"
#include "c6_ble_nimble.h"
#include "nimble/nimble_port.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_c6link.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_c6link.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_isr.h"
#include "ra8_time.h"
#include "tx_api.h"

static uint32_t     s_cpuclk_hz;
static uint32_t     s_pclka_hz;
static ra8_err_t    s_init_err = k_ra8_err_not_initialized;
static TX_THREAD    s_host_thread;
static TX_THREAD    s_pump_thread;
static TX_THREAD    s_beat_thread;
static CHAR         s_host_name[] = {'n', 'i', 'm', 'b', 'l', 'e', '_', 'h', 's', '\0'};
static CHAR         s_pump_name[] = {'h', 'c', 'i', '_', 'p', 'u', 'm', 'p', '\0'};
static CHAR         s_beat_name[] = {'n', 'i', 'm', 'b', 'l', 'e', '_', 'h', 'b', '\0'};
static UCHAR        s_host_stack[k_c6_ble_nimble_host_stack];
static UCHAR        s_pump_stack[k_c6_ble_nimble_pump_stack];
static UCHAR        s_beat_stack[k_c6_ble_nimble_beat_stack];
static uint8_t      s_arena[k_c6_ble_nimble_arena_bytes];
static ra8_c6link_t s_link;

RA8_INTERNAL static void internal_panic_halt(void)
{
  while (true) {
    __asm__ volatile("wfi");
  }
}

RA8_INTERNAL static void internal_setup_or_halt(void)
{
  const ra8_board_bringup_cfg_t bringup_cfg = {
    .console_baud = (uint32_t)k_c6_ble_nimble_uart_baud,
  };
  ra8_board_bringup_out_t bringup_out = {};
  if (ra8_board_bringup(&bringup_cfg, &bringup_out) != k_ra8_ok) {
    internal_panic_halt();
  }
  s_cpuclk_hz = bringup_out.cpuclk0_hz;
  s_pclka_hz  = bringup_out.pclka_hz;
}

RA8_INTERNAL static void
internal_log_notify(void* ctx, c6_ble_nimble_evt_t evt, const c6_ble_nimble_state_t* st)
{
  (void)ctx;
  if (st == nullptr) {
    return;
  }

  c6_ble_nimble_console_lock();
  switch (evt) {
    case k_c6_ble_nimble_evt_sync_ok:
      c6_ble_nimble_puts("c6_nimble: sync_ok bd_addr=");
      c6_ble_nimble_put_bd_addr_le(st->bd_addr);
      c6_ble_nimble_puts(" own_addr_type=");
      c6_ble_nimble_put_u32((uint32_t)st->own_addr_type);
      c6_ble_nimble_puts("\r\n");
      break;

    case k_c6_ble_nimble_evt_adv_ok:
      c6_ble_nimble_puts("c6_nimble: adv_start=ok name=EK-RA8D2\r\n");
      c6_ble_nimble_puts("c6_nimble: PASS nimble_c6_peripheral ready (advertising)\r\n");
      break;

    case k_c6_ble_nimble_evt_adv_fail:
      c6_ble_nimble_puts("c6_nimble: FAIL sync/adv status=");
      c6_ble_nimble_put_i32(st->last_status);
      c6_ble_nimble_puts("\r\n");
      break;

    case k_c6_ble_nimble_evt_connected:
      c6_ble_nimble_puts("c6_nimble: PASS central connected conn_handle=");
      c6_ble_nimble_put_u32((uint32_t)st->conn_handle);
      c6_ble_nimble_puts(" peer_addr=");
      c6_ble_nimble_put_bd_addr_le(st->peer_addr);
      c6_ble_nimble_puts("\r\n");
      break;

    case k_c6_ble_nimble_evt_conn_fail:
      c6_ble_nimble_puts("c6_nimble: connect failed status=");
      c6_ble_nimble_put_i32(st->last_status);
      c6_ble_nimble_puts("; restarting adv\r\n");
      break;

    case k_c6_ble_nimble_evt_disconn:
      c6_ble_nimble_puts("c6_nimble: disconnected conn_handle=");
      c6_ble_nimble_put_u32((uint32_t)st->conn_handle);
      c6_ble_nimble_puts(" reason=");
      c6_ble_nimble_put_i32(st->last_status);
      c6_ble_nimble_puts("; restarting adv\r\n");
      break;

    case k_c6_ble_nimble_evt_subscribe:
      c6_ble_nimble_puts("c6_nimble: subscribe attr_handle=");
      c6_ble_nimble_put_u32((uint32_t)st->subscribe_attr_handle);
      c6_ble_nimble_puts(" count=");
      c6_ble_nimble_put_u32(st->subscribe_count);
      c6_ble_nimble_puts("\r\n");
      break;

    case k_c6_ble_nimble_evt_mtu:
      c6_ble_nimble_puts("c6_nimble: mtu=");
      c6_ble_nimble_put_u32((uint32_t)st->mtu);
      c6_ble_nimble_puts(" conn_handle=");
      c6_ble_nimble_put_u32((uint32_t)st->conn_handle);
      c6_ble_nimble_puts("\r\n");
      break;

    case k_c6_ble_nimble_evt_host_reset:
      c6_ble_nimble_puts("c6_nimble: host reset reason=");
      c6_ble_nimble_put_i32(st->reset_reason);
      c6_ble_nimble_puts("\r\n");
      break;
  }
  c6_ble_nimble_console_unlock();
}

RA8_INTERNAL static void internal_pump_entry(ULONG arg)
{
  (void)arg;
  while (true) {
    (void)ble_hci_ra8_ble_dispatch();
    (void)tx_thread_sleep((ULONG)k_c6_ble_nimble_pump_sleep_ms);
  }
}

RA8_INTERNAL static void internal_beat_entry(ULONG arg)
{
  (void)arg;
  uint32_t beat = 0U;
  while (true) {
    c6_ble_nimble_state_t st = {};
    c6_ble_nimble_get_state(&st);
    c6_ble_nimble_console_lock();
    c6_ble_nimble_puts("c6_nimble: heartbeat n=");
    c6_ble_nimble_put_u32(beat);
    c6_ble_nimble_puts(" synced=");
    c6_ble_nimble_put_u32(st.synced ? 1U : 0U);
    c6_ble_nimble_puts(" adv=");
    c6_ble_nimble_put_u32(st.advertising ? 1U : 0U);
    c6_ble_nimble_puts(" conn=");
    c6_ble_nimble_put_u32(st.connected ? 1U : 0U);
    c6_ble_nimble_puts(" connects=");
    c6_ble_nimble_put_u32(st.connect_count);
    c6_ble_nimble_puts("\r\n");
    c6_ble_nimble_console_unlock();
    beat++;
    (void)tx_thread_sleep((ULONG)k_c6_ble_nimble_heartbeat_ms);
  }
}

RA8_INTERNAL static void internal_host_entry(ULONG arg)
{
  (void)arg;

  c6_ble_nimble_console_lock();
  c6_ble_nimble_puts("c6_nimble: port_init=");
  c6_ble_nimble_puts(ra8_err_to_str(s_init_err));
  c6_ble_nimble_puts("\r\n");
  c6_ble_nimble_console_unlock();
  if (s_init_err != k_ra8_ok) {
    c6_ble_nimble_puts("c6_nimble: FAIL port init failed\r\n");
    return;
  }

  ra8_delay_ms((uint32_t)k_c6_ble_nimble_boot_wait_ms);

  c6_ble_nimble_link_cfg_t lcfg = {
    .link        = &s_link,
    .arena       = s_arena,
    .arena_bytes = (uint32_t)sizeof(s_arena),
  };
  if (ra8_esp_hosted_c6link_bind(&lcfg.transport) != k_ra8_ok) {
    c6_ble_nimble_puts("c6_nimble: FAIL esp_hosted c6link bind failed\r\n");
    return;
  }

  if (!c6_ble_nimble_bind_link(&lcfg)) {
    c6_ble_nimble_state_t st = {};
    c6_ble_nimble_get_state(&st);
    const ra8_err_t fail_err =
      !st.link_opened ? st.open_err : (!st.ready_ok ? st.ready_err : st.ble_err);
    c6_ble_nimble_puts("c6_nimble: FAIL transport setup err=");
    c6_ble_nimble_puts(ra8_err_to_str(fail_err));
    c6_ble_nimble_puts("\r\n");
    return;
  }

  c6_ble_nimble_state_t fw_st = {};
  c6_ble_nimble_get_state(&fw_st);
  c6_ble_nimble_puts("c6_nimble: coprocessor fw=");
  c6_ble_nimble_put_u32(fw_st.fw.major);
  c6_ble_nimble_puts(".");
  c6_ble_nimble_put_u32(fw_st.fw.minor);
  c6_ble_nimble_puts(".");
  c6_ble_nimble_put_u32(fw_st.fw.patch);
  c6_ble_nimble_puts("\r\n");

  const c6_ble_nimble_app_cfg_t acfg = {
    .device_name         = "EK-RA8D2",
    .initial_battery_pct = (uint8_t)k_c6_ble_nimble_battery_init,
    .notify_cb           = internal_log_notify,
    .notify_ctx          = nullptr,
  };
  if (!c6_ble_nimble_stack_init(&acfg)) {
    c6_ble_nimble_puts("c6_nimble: FAIL NimBLE stack init failed\r\n");
    return;
  }

  if (tx_thread_create(&s_pump_thread,
                       s_pump_name,
                       internal_pump_entry,
                       0U,
                       s_pump_stack,
                       (ULONG)sizeof(s_pump_stack),
                       (UINT)k_c6_ble_nimble_pump_prio,
                       (UINT)k_c6_ble_nimble_pump_prio,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    c6_ble_nimble_puts("c6_nimble: FAIL pump thread create\r\n");
    return;
  }

  if (tx_thread_create(&s_beat_thread,
                       s_beat_name,
                       internal_beat_entry,
                       0U,
                       s_beat_stack,
                       (ULONG)sizeof(s_beat_stack),
                       (UINT)k_c6_ble_nimble_beat_prio,
                       (UINT)k_c6_ble_nimble_beat_prio,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    c6_ble_nimble_puts("c6_nimble: FAIL heartbeat thread create\r\n");
    return;
  }

  ra8_esp_hosted_mem_dump("c6_ble_nimble");
  nimble_port_run();
}

void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  c6_ble_nimble_console_lock_init();

  const ra8_esp_hosted_port_cfg_t cfg = {
    .pclk_hz      = s_pclka_hz,
    .sck_hz       = (uint32_t)k_c6_ble_nimble_sck_hz,
    .edge_poll_ms = (uint16_t)k_c6_ble_nimble_edge_poll_ms,
    .sci_channel  = (uint8_t)k_ra8_board_pmod1_sci_channel,
  };
  s_init_err = ra8_esp_hosted_port_init(&cfg);

  if (tx_thread_create(&s_host_thread,
                       s_host_name,
                       internal_host_entry,
                       0U,
                       s_host_stack,
                       (ULONG)sizeof(s_host_stack),
                       (UINT)k_c6_ble_nimble_host_prio,
                       (UINT)k_c6_ble_nimble_host_prio,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    c6_ble_nimble_puts("c6_nimble: FAIL host thread create\r\n");
  }
}

void main(void)
{
  internal_setup_or_halt();
  ra8_isr_globals_enable();

  c6_ble_nimble_print_banner(s_cpuclk_hz, s_pclka_hz);
  c6_ble_nimble_puts("c6_nimble: entering ThreadX\r\n");

  tx_kernel_enter();

  internal_panic_halt();
}
