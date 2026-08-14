/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_camera_livestream/main.c
 * @brief OV5640 browser livestream over the ESP32-C6 esp-hosted link.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details Initializes external SDRAM for the camera and JPEG buffers, then
 * joins Wi-Fi through the
 * bench-validated raw `ra8_c6link` sequence, obtains a NetX DHCP lease, then
 * serves a small browser UI and fresh QVGA JPEG frames on TCP port 80.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "c6_cam_credentials.h"
#include "c6_camera_livestream.h"
#include "nx_ether_driver_c6.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_c6link.h"
#include "ra8_c6link_wifi.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_c6link.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_sdramc.h"
#include "ra8_time.h"
#include "tx_api.h"

static uint32_t          s_cpuclk_hz;
static uint32_t          s_pclka_hz;
static ra8_err_t         s_port_err = k_ra8_err_not_initialized;
static TX_THREAD         s_worker;
static CHAR              s_worker_name[] = "c6_cam_stream";
static UCHAR             s_worker_stack[k_c6_cam_worker_stack];
static uint8_t           s_arena[k_c6_cam_arena_bytes];
static ra8_c6link_t      s_link;
static volatile uint32_t s_events;
static volatile uint8_t  s_connected;
static volatile uint8_t  s_disconnected;
static volatile uint16_t s_disconnect_reason;

static void c6_cam_halt(void)
{
  while (true) {
    __asm volatile("wfi");
  }
}

static void c6_cam_setup_or_halt(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    c6_cam_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &s_cpuclk_hz) != k_ra8_ok) {
    c6_cam_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &s_pclka_hz) != k_ra8_ok) {
    c6_cam_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    c6_cam_halt();
  }
  if (ra8_time_init(s_cpuclk_hz) != k_ra8_ok) {
    c6_cam_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_c6_cam_uart_baud) != k_ra8_ok) {
    c6_cam_halt();
  }
  c6_cam_puts("c6_cam: entering C6 handshake\r\n");
}

static void c6_cam_on_event(void* ctx, const ra8_c6link_event_t* event)
{
  (void)ctx;
  if (event == nullptr) {
    return;
  }
  s_events++;
  if (event->kind == k_ra8_c6link_event_sta_connected) {
    s_connected = 1U;
  } else if (event->kind == k_ra8_c6link_event_sta_disconnected) {
    s_disconnected      = 1U;
    s_disconnect_reason = event->reason;
  } else {
    /* Boot and bare Wi-Fi events are counted but need no state latch. */
  }
}

static void c6_cam_report_fault(const char* stage, ra8_err_t err)
{
  c6_cam_puts("c6_cam: FAIL ");
  c6_cam_puts(stage);
  c6_cam_puts(" err=");
  c6_cam_puts(ra8_err_to_str(err));
  c6_cam_puts("\r\n");
}

static ra8_err_t c6_cam_open_link(void)
{
  ra8_c6link_cfg_t cfg = {};
  ra8_err_t        err = ra8_esp_hosted_c6link_bind(&cfg.transport);
  if (err != k_ra8_ok) {
    return err;
  }
  cfg.arena       = s_arena;
  cfg.arena_bytes = (uint32_t)sizeof(s_arena);
  cfg.event_cb    = c6_cam_on_event;
  cfg.rx_cb       = nx_ether_driver_c6_rx;
  cfg.cb_ctx      = nullptr;
  return ra8_c6link_open(&s_link, &cfg);
}

static bool c6_cam_wait_connected(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_c6_cam_assoc_polls; i++) {
    (void)ra8_c6link_poll(&s_link, (uint16_t)k_ra8_c6link_announce_transfers, nullptr);
    if (s_connected != 0U) {
      return true;
    }
    if (s_disconnected != 0U) {
      return false;
    }
    tx_thread_sleep((ULONG)k_c6_cam_assoc_gap_ms);
  }
  return false;
}

static bool c6_cam_prepare_link(void)
{
  ra8_err_t err = c6_cam_open_link();
  c6_cam_puts("c6_cam: link_open=");
  c6_cam_puts(ra8_err_to_str(err));
  c6_cam_puts("\r\n");
  if (err != k_ra8_ok) {
    return false;
  }
  ra8_c6link_fw_version_t version = {};
  err = ra8_c6link_await_ready(&s_link, (uint16_t)k_ra8_c6link_announce_transfers, &version);
  if (err != k_ra8_ok) {
    c6_cam_report_fault("await_ready", err);
    return false;
  }
  c6_cam_puts("c6_cam: coprocessor fw=");
  c6_cam_put_u32(version.major);
  c6_cam_puts(".");
  c6_cam_put_u32(version.minor);
  c6_cam_puts(".");
  c6_cam_put_u32(version.patch);
  c6_cam_puts("\r\n");
  return true;
}

static bool c6_cam_join(c6_cam_lease_t* lease)
{
  if (g_c6_cam_wifi_ssid[0] == '\0') {
    c6_cam_puts("c6_cam: FAIL no Wi-Fi credentials compiled in\r\n");
    return false;
  }

  ra8_err_t err = ra8_c6link_wifi_start(&s_link);
  if (err != k_ra8_ok) {
    c6_cam_report_fault("wifi_start", err);
    return false;
  }
  ra8_c6link_mac_t mac = {};
  err                  = ra8_c6link_wifi_mac(&s_link, &mac);
  if (err != k_ra8_ok) {
    c6_cam_report_fault("wifi_mac", err);
    return false;
  }
  ra8_c6link_sta_cfg_t station = {};
  err = ra8_c6link_sta_cfg_set(&station, g_c6_cam_wifi_ssid, g_c6_cam_wifi_psk);
  if (err != k_ra8_ok) {
    c6_cam_report_fault("sta_cfg_set", err);
    return false;
  }
  s_connected         = 0U;
  s_disconnected      = 0U;
  s_disconnect_reason = 0U;
  err                 = ra8_c6link_wifi_join(&s_link, &station);
  if (err != k_ra8_ok) {
    c6_cam_report_fault("wifi_join", err);
    return false;
  }
  if (!c6_cam_wait_connected()) {
    c6_cam_puts("c6_cam: FAIL association reason=");
    c6_cam_put_u32((uint32_t)s_disconnect_reason);
    c6_cam_puts(" events=");
    c6_cam_put_u32((uint32_t)s_events);
    c6_cam_puts("\r\n");
    return false;
  }
  c6_cam_puts("c6_cam: associated events=");
  c6_cam_put_u32((uint32_t)s_events);
  c6_cam_puts("\r\n");

  err = c6_cam_net_up(&s_link, &mac, lease);
  if ((err != k_ra8_ok) || !lease->bound) {
    c6_cam_puts("c6_cam: FAIL DHCP err=");
    c6_cam_puts(ra8_err_to_str(err));
    c6_cam_puts("\r\n");
    return false;
  }
  return true;
}

static void c6_cam_worker_entry(ULONG input)
{
  (void)input;
  if (s_port_err != k_ra8_ok) {
    c6_cam_puts("c6_cam: FAIL esp-hosted port init err=");
    c6_cam_puts(ra8_err_to_str(s_port_err));
    c6_cam_puts("\r\n");
    c6_cam_halt();
  }
  ra8_delay_ms((uint32_t)k_c6_cam_boot_wait_ms);
  if (!c6_cam_prepare_link()) {
    c6_cam_halt();
  }
  c6_cam_lease_t lease = {};
  if (!c6_cam_join(&lease)) {
    c6_cam_halt();
  }
  c6_cam_puts("c6_cam: PASS Wi-Fi and DHCP ip=");
  c6_cam_put_ip(lease.ip);
  c6_cam_puts("\r\n");
  if (ra8_sdramc_init() != k_ra8_ok) {
    c6_cam_puts("c6_cam: FAIL SDRAM init\r\n");
    c6_cam_halt();
  }
  c6_cam_puts("c6_cam: sdram=PASS\r\n");
  const ra8_err_t camera = c6_cam_camera_init();
  if (camera != k_ra8_ok) {
    c6_cam_puts("c6_cam: FAIL camera init err=");
    c6_cam_puts(ra8_err_to_str(camera));
    c6_cam_puts("\r\n");
    c6_cam_halt();
  }
  c6_cam_puts("c6_cam: camera=PASS frame=640x480 UYVY stream=320x240 JPEG\r\n");
  c6_cam_puts("\r\nc6_cam: open http://");
  c6_cam_put_ip(lease.ip);
  c6_cam_puts("/\r\n");
  c6_cam_http_serve();
}

void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;
  const ra8_esp_hosted_port_cfg_t cfg = {
    .pclk_hz      = s_pclka_hz,
    .sck_hz       = (uint32_t)k_c6_cam_sck_hz,
    .edge_poll_ms = (uint16_t)k_c6_cam_edge_poll_ms,
    .sci_channel  = (uint8_t)k_ra8_board_pmod1_sci_channel,
  };
  s_port_err = ra8_esp_hosted_port_init(&cfg);
  if (tx_thread_create(&s_worker,
                       s_worker_name,
                       c6_cam_worker_entry,
                       0U,
                       s_worker_stack,
                       (ULONG)sizeof(s_worker_stack),
                       (UINT)k_c6_cam_worker_prio,
                       (UINT)k_c6_cam_worker_prio,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    c6_cam_puts("c6_cam: FAIL worker create\r\n");
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  c6_cam_setup_or_halt();
  ra8_isr_globals_enable();
  c6_cam_puts("c6_cam: entering ThreadX\r\n");
  tx_kernel_enter();
  c6_cam_halt();
  return 0;
}
#pragma GCC diagnostic pop
