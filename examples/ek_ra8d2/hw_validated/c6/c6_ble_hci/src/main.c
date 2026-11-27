/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_ble_hci/src/main.c
 * @brief Exercise `ra8_ble` over `ra8_c6link` ESP32-C6 SPI (`ble_hci_c6link`).
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

#include "c6_ble_hci.h"
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
static TX_THREAD    s_worker;
static CHAR         s_worker_name[] = "c6_ble_hci";
static UCHAR        s_worker_stack[k_c6_ble_worker_stack];
static uint8_t      s_arena[k_c6_ble_arena_bytes];
static ra8_c6link_t s_link;

static void c6_ble_panic_halt(void)
{
  while (true) {
    __asm__ volatile("wfi");
  }
}

static void c6_ble_setup_or_halt(void)
{
  const ra8_board_bringup_cfg_t bringup_cfg = {
    .console_baud = (uint32_t)k_c6_ble_uart_baud,
  };
  ra8_board_bringup_out_t bringup_out = {};
  if (ra8_board_bringup(&bringup_cfg, &bringup_out) != k_ra8_ok) {
    c6_ble_panic_halt();
  }
  s_cpuclk_hz = bringup_out.cpuclk0_hz;
  s_pclka_hz  = bringup_out.pclka_hz;
}

static void c6_ble_wait_cb(void* ctx, uint32_t ms)
{
  (void)ctx;
  ra8_delay_ms(ms);
}

static void c6_ble_heartbeat(bool passed)
{
  uint32_t beat = 0U;
  while (true) {
    c6_ble_puts("c6_ble: heartbeat n=");
    c6_ble_put_u32(beat);
    c6_ble_puts(passed ? " verdict=PASS\r\n" : " verdict=FAIL\r\n");
    beat++;
    tx_thread_sleep((ULONG)k_c6_ble_heartbeat_ms);
  }
}

static void c6_ble_report_result(const c6_ble_result_t* res)
{
  if (res->ready_ok) {
    c6_ble_puts("c6_ble: coprocessor fw=");
    c6_ble_put_u32(res->fw.major);
    c6_ble_puts(".");
    c6_ble_put_u32(res->fw.minor);
    c6_ble_puts(".");
    c6_ble_put_u32(res->fw.patch);
    c6_ble_puts(" chip_id=0x");
    c6_ble_put_hex(res->fw.chip_id, (uint8_t)k_c6_ble_hex_byte);
    c6_ble_puts("\r\n");
  }
  c6_ble_puts("c6_ble: hci_reset=");
  c6_ble_puts(res->reset_ok ? "ok" : "fail");
  if (res->bd_addr_ok) {
    c6_ble_puts(" bd_addr=");
    c6_ble_put_bd_addr(res->bd_addr);
  }
  c6_ble_puts(" adv=");
  c6_ble_puts(res->adv_enable_ok ? "ok" : "fail");
  c6_ble_puts(" events=");
  c6_ble_put_u32(res->events_seen);
  c6_ble_puts("\r\n");
  if (res->passed) {
    c6_ble_puts("c6_ble: PASS ble_hci_c6link reset, bd_addr, and advertising accepted\r\n");
    return;
  }
  const ra8_err_t fail_err =
    !res->link_opened ? res->open_err : (!res->ready_ok ? res->ready_err : res->cmd_err);
  c6_ble_puts("c6_ble: FAIL err=");
  c6_ble_puts(ra8_err_to_str(fail_err));
  c6_ble_puts(" hci_status=0x");
  c6_ble_put_hex((uint32_t)res->hci_status, (uint8_t)k_c6_ble_hex_byte);
  c6_ble_puts("\r\n");
}

static void c6_ble_worker_entry(ULONG thread_input)
{
  (void)thread_input;

  c6_ble_puts("c6_ble: port_init=");
  c6_ble_puts(ra8_err_to_str(s_init_err));
  c6_ble_puts("\r\n");
  if (s_init_err != k_ra8_ok) {
    c6_ble_puts("c6_ble: FAIL port init failed -- no HCI frame sent\r\n");
    c6_ble_heartbeat(false);
    return;
  }

  ra8_delay_ms((uint32_t)k_c6_ble_boot_wait_ms);

  c6_ble_run_cfg_t rcfg = {
    .link        = &s_link,
    .arena       = s_arena,
    .arena_bytes = (uint32_t)sizeof(s_arena),
    .wait_ms     = c6_ble_wait_cb,
    .wait_ctx    = nullptr,
    .poll_budget = (uint16_t)k_c6_ble_cmd_poll_budget,
  };
  const ra8_err_t bound = ra8_esp_hosted_c6link_bind(&rcfg.transport);
  if (bound != k_ra8_ok) {
    c6_ble_puts("c6_ble: FAIL transport bind err=");
    c6_ble_puts(ra8_err_to_str(bound));
    c6_ble_puts("\r\n");
    c6_ble_heartbeat(false);
    return;
  }

  c6_ble_result_t res = {};
  const bool      ok  = c6_ble_hci_run(&rcfg, &res);
  c6_ble_report_result(&res);
  ra8_esp_hosted_mem_dump("c6_ble_hci");
  c6_ble_heartbeat(ok);
}

void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  const ra8_esp_hosted_port_cfg_t cfg = {
    .pclk_hz      = s_pclka_hz,
    .sck_hz       = (uint32_t)k_c6_ble_sck_hz,
    .edge_poll_ms = (uint16_t)k_c6_ble_edge_poll_ms,
    .sci_channel  = (uint8_t)k_ra8_board_pmod1_sci_channel,
  };
  s_init_err = ra8_esp_hosted_port_init(&cfg);

  if (tx_thread_create(&s_worker,
                       s_worker_name,
                       c6_ble_worker_entry,
                       0U,
                       s_worker_stack,
                       (ULONG)sizeof(s_worker_stack),
                       (UINT)k_c6_ble_worker_prio,
                       (UINT)k_c6_ble_worker_prio,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    c6_ble_puts("c6_ble: worker thread creation failed\r\n");
  }
}

void main(void)
{
  c6_ble_setup_or_halt();
  ra8_isr_globals_enable();

  c6_ble_print_banner(s_cpuclk_hz, s_pclka_hz);
  c6_ble_puts("c6_ble: entering ThreadX; port init runs from tx_application_define\r\n");

  tx_kernel_enter();

  c6_ble_panic_halt();
}
