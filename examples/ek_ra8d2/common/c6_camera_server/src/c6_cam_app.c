/**
 * @file examples/ek_ra8d2/common/c6_camera_server/src/c6_cam_app.c
 * @brief Shared C6 camera-stream application workflow.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details Initializes external SDRAM for camera storage, joins Wi-Fi through
 * the bench-validated raw `ra8_c6link` sequence, obtains a NetX DHCP lease,
 * then serves the selected camera backend on TCP port 80. Each camera example
 * supplies its own local `main.c` and camera backend while reusing this flow.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "c6_camera_server.h"
#include "c6_camera_server_credentials.h"
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

/**
 * @brief Park the application after a terminal failure.
 * @details Repeatedly waits for interrupt and never returns to the failed workflow.
 * @pre A terminal condition has been diagnosed or the server has returned unexpectedly.
 * @pre Interrupt state is suitable for the application terminal state.
 * @post No further workflow stage executes.
 * @post Static application state remains available to a debugger.
 * @note Only reset or debugger intervention terminates the loop.
 * @since 0.1.0
 */
static void c6_cam_halt(void)
{
  while (true) {
    __asm volatile("wfi");
  }
}

/**
 * @brief Initialize shared clocks, services, and the diagnostic console.
 * @details Performs ordered CGC, MSTP, ISR, timebase, and UART initialization.
 * @pre Startup initialized data, BSS, vector state, and the CPU clock source.
 * @pre Called from the single-threaded application entry path.
 * @post On success, clock globals and the diagnostic console are ready.
 * @post Any initialization failure transfers permanently to `c6_cam_halt`.
 * @note ThreadX has not started when this function runs.
 * @since 0.1.0
 */
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
  if (ra8_isr_init() != k_ra8_ok) {
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

/**
 * @brief Latch station connection events from the C6 link.
 * @details Counts every delivered event and records connected or disconnected state.
 * @param[in,out] ctx Unused callback context.
 * @param[in] event Borrowed event descriptor from the C6 link.
 * @pre Static event latches are writable.
 * @pre The callback executes serially with C6 link polling.
 * @post Nonnull events increment the event count once.
 * @post Connection transitions update their corresponding latches.
 * @note Boot and unrelated Wi-Fi events are counted without extra state.
 * @since 0.1.0
 */
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

/**
 * @brief Emit one standardized application failure line.
 * @details Combines a stage label with the repository error string on SCI8.
 * @param[in] stage NUL-terminated failing-stage name.
 * @param[in] err Repository error code to describe.
 * @pre `stage` is nonnull and bounded for `c6_cam_puts`.
 * @pre The diagnostic console is initialized.
 * @post One complete failure line is queued.
 * @post Application workflow state remains unchanged.
 * @note This helper does not halt the caller.
 * @since 0.1.0
 */
static void c6_cam_report_fault(const char* stage, ra8_err_t err)
{
  c6_cam_puts("c6_cam: FAIL ");
  c6_cam_puts(stage);
  c6_cam_puts(" err=");
  c6_cam_puts(ra8_err_to_str(err));
  c6_cam_puts("\r\n");
}

/**
 * @brief Bind and open the raw ESP-hosted C6 link.
 * @details Supplies static arena storage plus event and NetX receive callbacks.
 * @return Repository error code.
 * @retval k_ra8_ok The link opened successfully.
 * @retval k_ra8_err_not_initialized The ESP-hosted transport was unavailable.
 * @pre The ESP-hosted port configuration is available to its transport binder.
 * @pre Static link and arena storage are not in use by another link.
 * @post On success, `s_link` owns the active transport session.
 * @post The arena remains statically allocated for the session lifetime.
 * @note Transport or link-open errors are propagated unchanged.
 * @since 0.1.0
 */
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

/**
 * @brief Poll for a station association result.
 * @details Services C6 transfers between bounded sleeps until connected, disconnected, or timed out.
 * @return Association outcome.
 * @retval true A station-connected event was observed.
 * @retval false Disconnection or the poll budget ended first.
 * @pre `s_link` is open and Wi-Fi join was requested.
 * @pre ThreadX timing services are running.
 * @post All observed C6 events have been dispatched through the event callback.
 * @post The function returns within the configured poll budget.
 * @note Poll transport errors are ignored while event latches determine the result.
 * @since 0.1.0
 */
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

/**
 * @brief Open the C6 link and await firmware readiness.
 * @details Reports link status and the coprocessor firmware version to SCI8.
 * @return Link readiness outcome.
 * @retval true The link opened and the ready announcement arrived.
 * @retval false Transport open or ready negotiation failed.
 * @pre The ESP-hosted port initialization result is successful.
 * @pre The diagnostic console is initialized.
 * @post On success, `s_link` is ready for Wi-Fi control commands.
 * @post Failures are reported before returning false.
 * @note The caller decides whether failure is terminal.
 * @since 0.1.0
 */
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

/**
 * @brief Join configured Wi-Fi and acquire a DHCP lease.
 * @details Starts station mode, reads the MAC, associates, and binds NetX to the link.
 * @param[out] lease Destination for the bound network lease.
 * @return Join outcome.
 * @retval true Association and DHCP completed with a bound lease.
 * @retval false Credentials, control, association, or DHCP failed.
 * @pre `s_link` is open and ready for Wi-Fi commands.
 * @pre `lease` points to writable storage.
 * @post On success, `lease->bound` is true and address fields are valid.
 * @post Failure diagnostics identify the stage or association reason.
 * @note Credentials are compiled into a generated translation unit.
 * @since 0.1.0
 */
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

/**
 * @brief Initialize and self-test the audio and camera backends.
 * @details Proves local media hardware before any Wi-Fi or DHCP dependency can
 *          mask a camera failure, while giving the C6 additional boot time.
 * @return Whether every media backend passed its startup contract.
 * @retval true SDRAM, audio, camera initialization, and one JPEG capture passed.
 * @retval false A diagnostic was emitted for the failing stage.
 * @pre ThreadX and the shared clock services are initialized.
 * @pre No network server is using the media backends.
 * @post On success, audio streaming is active and the camera is capture-ready.
 * @post On failure, the caller may halt without exposing an HTTP endpoint.
 * @note The startup JPEG remains owned by the selected camera backend.
 * @since 0.1.0
 */
static bool c6_cam_prepare_media(void)
{
  if (ra8_sdramc_init() != k_ra8_ok) {
    c6_cam_puts("c6_cam: FAIL SDRAM init\r\n");
    return false;
  }
  c6_cam_puts("c6_cam: sdram=PASS\r\n");
  const ra8_err_t audio = c6_cam_audio_start();
  if (audio != k_ra8_ok) {
    c6_cam_puts("c6_cam: FAIL audio init err=");
    c6_cam_puts(ra8_err_to_str(audio));
    c6_cam_puts("\r\n");
    return false;
  }
  c6_cam_puts("c6_cam: audio=PASS mic=MIC1 fs=16000Hz pcm=20bit\r\n");
  const ra8_err_t camera = c6_cam_camera_init();
  if (camera != k_ra8_ok) {
    c6_cam_puts("c6_cam: FAIL camera init err=");
    c6_cam_puts(ra8_err_to_str(camera));
    c6_cam_camera_report_last_error();
    c6_cam_puts("\r\n");
    return false;
  }
  const uint8_t*        startup_jpeg       = nullptr;
  uint32_t              startup_jpeg_bytes = 0U;
  c6_cam_frame_timing_t startup_timing     = {};
  const ra8_err_t       startup_capture =
    c6_cam_camera_capture_jpeg(&startup_jpeg, &startup_jpeg_bytes, &startup_timing);
  if (startup_capture != k_ra8_ok) {
    c6_cam_puts("c6_cam: FAIL camera selftest err=");
    c6_cam_puts(ra8_err_to_str(startup_capture));
    c6_cam_camera_report_last_error();
    c6_cam_puts("\r\n");
    return false;
  }
  c6_cam_puts("c6_cam: camera selftest=PASS jpeg_bytes=");
  c6_cam_put_u32(startup_jpeg_bytes);
  c6_cam_puts(" capture_ms=");
  c6_cam_put_u32(startup_timing.capture_ms);
  c6_cam_puts("\r\n");
  c6_cam_puts("c6_cam: camera=PASS ");
  c6_cam_puts(c6_cam_camera_description());
  c6_cam_puts("\r\n");
  return true;
}

/**
 * @brief Run the network, audio, camera, and HTTP worker workflow.
 * @details Validates local media, joins Wi-Fi, then serves HTTP.
 * @param[in] input ThreadX entry parameter, unused by this worker.
 * @pre ThreadX created the worker with its static stack.
 * @pre `tx_application_define` stored the ESP-hosted port result.
 * @post On success, control transfers permanently to the HTTP server.
 * @post Any failure emits a diagnostic and parks the worker.
 * @note All persistent buffers are static or caller-owned.
 * @since 0.1.0
 */
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
  if (!c6_cam_prepare_media()) {
    c6_cam_halt();
  }
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

int32_t c6_cam_app_run(void)
{
  c6_cam_setup_or_halt();
  ra8_isr_globals_enable();
  c6_cam_puts("c6_cam: entering ThreadX\r\n");
  tx_kernel_enter();
  c6_cam_halt();
  return 0;
}
