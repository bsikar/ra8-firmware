/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_wifi_join/src/main.c
 * @brief Join the bench Wi-Fi over the ESP32-C6 and get an IP by DHCP (#492).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The first application on this board that takes a network all the way up over
 * the ESP32-C6: it associates the co-processor's Wi-Fi station with a bench
 * access point through ``ra8_c6link``, then runs a NetX Duo DHCP client over
 * the ``nx_ether_driver_c6`` link driver to obtain a lease, and finally pings
 * the gateway to prove the path carries traffic.
 *
 * Two threads share the C6 link once the IP is up: the NetX IP thread transmits
 * through the driver, and the driver's own RX poll worker pumps the link for
 * received frames. Both are serialised behind the driver's wire mutex, so the
 * application worker does no direct ``ra8_c6link`` calls after
 * ::c6_join_net_up begins -- the driver owns the wire from there.
 *
 * The image contains no network credential. Its worker receives one bounded,
 * versioned provisioning record over the board console after boot and erases
 * that record immediately after the synchronous association request.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "c6_join.h"
#include "nx_ether_driver_c6.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_c6link.h"
#include "ra8_c6link_wifi.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_c6link.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_net_provision.h"
#include "ra8_secure.h"
#include "ra8_time.h"
#include "tx_api.h"

/** @brief Cached CPUCLK0 rate. @since 0.1.0 */
static uint32_t s_cpuclk_hz;
/** @brief Cached PCLKA rate. @since 0.1.0 */
static uint32_t s_pclka_hz;
/** @brief Port-init result from tx_application_define. @since 0.1.0 */
static ra8_err_t s_init_err = k_ra8_err_not_initialized;
/** @brief Application worker thread. @since 0.1.0 */
static TX_THREAD s_worker;
/** @brief Worker thread name. @since 0.1.0 */
static CHAR s_worker_name[] = "c6_wifi_join";
/** @brief Worker thread stack. @since 0.1.0 */
static UCHAR s_worker_stack[k_c6_join_worker_stack];
/** @brief Decode arena handed to the facade. @since 0.1.0 */
static uint8_t s_arena[k_c6_join_arena_bytes];
/** @brief The one C6 link handle this app owns. @since 0.1.0 */
static ra8_c6link_t s_link;
/** @brief Count of co-processor announcements seen. @since 0.1.0 */
static volatile uint32_t s_events;
/** @brief Non-zero once a station-connected event arrived. @since 0.1.0 */
static volatile uint8_t s_connected;
/** @brief Non-zero once a station-disconnected event arrived. @since 0.1.0 */
static volatile uint8_t s_disconnected;
/** @brief Last 802.11 disconnect reason code. @since 0.1.0 */
static volatile uint16_t s_reason;
/** @brief Board-console binding for the shared runtime provisioner. @since 0.1.0 */
static const ra8_net_provision_uart_t s_provision_uart = {
  .write   = ra8_board_uart_console_write,
  .read    = ra8_board_uart_console_read,
  .wait_ms = ra8_delay_ms,
};

/**
 * @brief Halt the processor after an unrecoverable start-up failure.
 * @details Executes wait-for-interrupt forever so a failed kernel or platform
 *          initialization cannot fall through into partially configured code.
 * @return Never returns.
 * @pre A terminal start-up failure was detected.
 * @pre Interrupt state is safe for an indefinite wait.
 * @post No application operation executes after entry.
 * @post The processor remains in the bounded halt loop.
 * @note Not thread-safe; this is a terminal single-context path.
 * @since 0.1.0
 */
static void c6_join_panic_halt(void)
{
  while (true) {
    __asm volatile("wfi");
  }
}

/**
 * @brief Initialize platform services required by the join application.
 * @details Brings up clocks, records live rates, enables module-stop control,
 *          initializes delay timing, and opens the console in dependency order.
 * @return Nothing after successful initialization; failures halt permanently.
 * @pre Reset-time clock and board state is available.
 * @pre No worker or C6 transaction has started.
 * @post Live CPUCLK0 and PCLKA rates are cached.
 * @post Timing and board-console services are initialized on return.
 * @note Not thread-safe; call exactly once during boot.
 * @since 0.1.0
 */
static void c6_join_setup_or_halt(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    c6_join_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &s_cpuclk_hz) != k_ra8_ok) {
    c6_join_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &s_pclka_hz) != k_ra8_ok) {
    c6_join_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    c6_join_panic_halt();
  }
  if (ra8_time_init(s_cpuclk_hz) != k_ra8_ok) {
    c6_join_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_c6_join_uart_baud) != k_ra8_ok) {
    c6_join_panic_halt();
  }
}

/**
 * @brief Record one C6 announcement and update association latches.
 * @details Counts every non-null event and records connected or disconnected
 *          station state while preserving the last disconnect reason.
 * @param[in] ctx Unused callback context.
 * @param[in] ev Event to record; null is ignored.
 * @return Nothing.
 * @pre The callback is installed on the application-owned C6 link.
 * @pre Event fields follow the C6 link contract when `ev` is non-null.
 * @post A non-null event increments the event counter once.
 * @post Station events update only their corresponding latches.
 * @note Called by the single C6 pump context; shared latches are volatile.
 * @since 0.1.0
 */
static void c6_join_on_event(void* ctx, const ra8_c6link_event_t* ev)
{
  (void)ctx;
  if (ev == nullptr) {
    return;
  }
  s_events++;
  if (ev->kind == k_ra8_c6link_event_sta_connected) {
    s_connected = 1U;
  } else if (ev->kind == k_ra8_c6link_event_sta_disconnected) {
    s_disconnected = 1U;
    s_reason       = ev->reason;
  } else {
    /* boot / bare wifi events: counted above, nothing else to latch. */
  }
}

/**
 * @brief Report one failed C6 join stage without revealing credentials.
 * @details Snapshots the link fault for diagnostic retention, then prints only
 *          the caller-supplied stage name and repository error description.
 * @param[in] what NUL-terminated non-secret stage name.
 * @param[in] err Repository error returned by that stage.
 * @return Nothing.
 * @pre The console is initialized.
 * @pre `what` is valid within the console string bound.
 * @post One failure line is emitted.
 * @post No credential or SSID bytes are transmitted.
 * @note Not thread-safe; the application worker is the sole caller.
 * @since 0.1.0
 */
static void c6_join_report_fault(const char* what, ra8_err_t err)
{
  ra8_c6link_fault_t fault = {};
  (void)ra8_c6link_last_fault(&s_link, &fault);
  c6_join_puts("c6_join: FAIL ");
  c6_join_puts(what);
  c6_join_puts(" err=");
  c6_join_puts(ra8_err_to_str(err));
  c6_join_puts("\r\n");
}

/**
 * @brief Bind ESP-hosted transport and open the application C6 link.
 * @details Builds the fixed-arena link configuration with event and NetX RX
 *          callbacks, then delegates transport ownership to ::ra8_c6link_open.
 * @return Repository error code.
 * @retval k_ra8_ok The transport was bound and link opened.
 * @retval other The bind or link-open error propagated unchanged.
 * @pre ESP-hosted platform initialization has completed.
 * @pre The static link and arena are not owned by another caller.
 * @post Success leaves ::s_link open with callbacks installed.
 * @post Bind failure leaves the link unopened.
 * @note Not thread-safe; called once by the worker.
 * @since 0.1.0
 */
static ra8_err_t c6_join_open_link(void)
{
  ra8_c6link_cfg_t cfg  = {};
  const ra8_err_t  seam = ra8_esp_hosted_c6link_bind(&cfg.transport);
  if (seam != k_ra8_ok) {
    return seam;
  }
  cfg.arena       = s_arena;
  cfg.arena_bytes = (uint32_t)sizeof(s_arena);
  cfg.event_cb    = c6_join_on_event;
  cfg.rx_cb       = nx_ether_driver_c6_rx;
  cfg.cb_ctx      = nullptr;
  return ra8_c6link_open(&s_link, &cfg);
}

/**
 * @brief Open the C6 link and establish co-processor readiness.
 * @details Opens the application link, waits for the bounded ready announcement,
 *          and emits only non-secret firmware and chip identity fields.
 * @param[out] out Firmware-version destination.
 * @return Whether the link reached ready state.
 * @retval true The link opened and readiness completed.
 * @retval false Link open or readiness failed.
 * @pre `out` is non-null and writable.
 * @pre The ESP-hosted port was initialized successfully.
 * @post Success initializes `out` and leaves the link open.
 * @post Failure emits a stage-specific diagnostic.
 * @note Not thread-safe; called once by the worker.
 * @since 0.1.0
 */
static bool c6_join_phase_ready(ra8_c6link_fw_version_t* out)
{
  const ra8_err_t opened = c6_join_open_link();
  c6_join_puts("c6_join: link_open=");
  c6_join_puts(ra8_err_to_str(opened));
  c6_join_puts("\r\n");
  if (opened != k_ra8_ok) {
    return false;
  }
  const ra8_err_t ready =
    ra8_c6link_await_ready(&s_link, (uint16_t)k_ra8_c6link_announce_transfers, out);
  if (ready != k_ra8_ok) {
    c6_join_report_fault("await_ready", ready);
    return false;
  }
  c6_join_puts("c6_join: coprocessor fw=");
  c6_join_put_u32(out->major);
  c6_join_puts(".");
  c6_join_put_u32(out->minor);
  c6_join_puts(".");
  c6_join_put_u32(out->patch);
  c6_join_puts(" chip_id=");
  c6_join_put_hex(out->chip_id, (uint8_t)k_c6_join_hex_byte);
  c6_join_puts("\r\n");
  return true;
}

/**
 * @brief Poll until station association succeeds, fails, or times out.
 * @details Pumps one bounded C6 announcement batch per iteration, checks the
 *          connection latches, and sleeps between attempts.
 * @return Association outcome.
 * @retval true A connected event arrived within the fixed budget.
 * @retval false A disconnected event arrived or the budget expired.
 * @pre The link is open and a join request was issued.
 * @pre The callback latches belong to this worker journey.
 * @post At most ::k_c6_join_assoc_tries polls occurred.
 * @post Link ownership remains with the caller.
 * @note Not thread-safe; the worker is the sole link-pump owner here.
 * @since 0.1.0
 */
static bool c6_join_wait_connected(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_c6_join_assoc_tries; i++) {
    ra8_c6link_stats_t stats = {};
    (void)ra8_c6link_poll(&s_link, (uint16_t)k_ra8_c6link_announce_transfers, &stats);
    if (s_connected != 0U) {
      return true;
    }
    if (s_disconnected != 0U) {
      return false;
    }
    tx_thread_sleep((ULONG)k_c6_join_assoc_gap_ms);
  }
  return false;
}

/**
 * @brief Start Wi-Fi and associate using one runtime credential record.
 * @details Starts station mode, reads the local MAC, builds a transient C6
 *          station request, submits it synchronously, erases that request, and
 *          waits for the association event without logging the SSID or PSK.
 * @param[out] mac_out Destination for the station MAC.
 * @param[in] credentials Validated runtime SSID and PSK record.
 * @return Association outcome.
 * @retval true The station associated within the fixed poll budget.
 * @retval false Start, MAC, config, join, or association failed.
 * @pre `mac_out` and `credentials` are non-null.
 * @pre The C6 link is open and ready.
 * @post The transient station configuration is securely erased.
 * @post Success initializes `mac_out`; failure emits a non-secret diagnostic.
 * @note Not thread-safe; called once by the worker.
 * @since 0.1.0
 */
static bool c6_join_phase_associate(ra8_c6link_mac_t*            mac_out,
                                    const ra8_net_credentials_t* credentials)
{
  const ra8_err_t started = ra8_c6link_wifi_start(&s_link);
  if (started != k_ra8_ok) {
    c6_join_report_fault("wifi_start", started);
    return false;
  }
  const ra8_err_t got_mac = ra8_c6link_wifi_mac(&s_link, mac_out);
  if (got_mac != k_ra8_ok) {
    c6_join_report_fault("wifi_mac", got_mac);
    return false;
  }
  c6_join_puts("c6_join: station mac=");
  c6_join_put_mac(mac_out);
  c6_join_puts("\r\n");

  ra8_c6link_sta_cfg_t sta = {};
  const ra8_err_t      set = ra8_c6link_sta_cfg_set(&sta, credentials->ssid, credentials->psk);
  if (set != k_ra8_ok) {
    ra8_secure_memzero(&sta, sizeof(sta));
    c6_join_report_fault("sta_cfg_set", set);
    return false;
  }
  const ra8_err_t joined = ra8_c6link_wifi_join(&s_link, &sta);
  ra8_secure_memzero(&sta, sizeof(sta));
  if (joined != k_ra8_ok) {
    c6_join_report_fault("wifi_join", joined);
    return false;
  }
  if (!c6_join_wait_connected()) {
    c6_join_puts("c6_join: FAIL association did not complete (reason=");
    c6_join_put_u32((uint32_t)s_reason);
    c6_join_puts(")\r\n");
    return false;
  }
  c6_join_puts("c6_join: associated\r\n");
  return true;
}

/**
 * @brief Bring up NetX, print the lease, and judge address acquisition.
 * @details Delegates DHCP binding to ::c6_join_net_up and emits only address,
 *          gateway, server, and reachability status fields.
 * @param[in] mac Associated station MAC address.
 * @return Whether DHCP produced a non-zero address.
 * @retval true A non-zero lease address was bound.
 * @retval false Network bring-up failed or returned a zero address.
 * @pre `mac` is non-null and initialized.
 * @pre Station association completed successfully.
 * @post Success prints the complete lease and ping status.
 * @post Failure prints a non-secret DHCP diagnostic.
 * @note Not thread-safe; the worker transfers link ownership to NetX here.
 * @since 0.1.0
 */
static bool c6_join_phase_ip(const ra8_c6link_mac_t* mac)
{
  c6_join_lease_t lease = {};
  const ra8_err_t up    = c6_join_net_up(&s_link, mac, &lease);
  if (up != k_ra8_ok) {
    c6_join_puts("c6_join: FAIL dhcp did not bind err=");
    c6_join_puts(ra8_err_to_str(up));
    c6_join_puts("\r\n");
    return false;
  }
  c6_join_puts("c6_join: dhcp bound ip=");
  c6_join_put_ip(lease.ip);
  c6_join_puts(" mask=");
  c6_join_put_ip(lease.mask);
  c6_join_puts(" gw=");
  c6_join_put_ip(lease.gateway);
  c6_join_puts(" server=");
  c6_join_put_ip(lease.dhcp_server);
  c6_join_puts("\r\n");
  c6_join_puts("c6_join: reachability gateway ping=");
  c6_join_puts(lease.ping_ok ? "ok" : "no-reply");
  c6_join_puts("\r\n");
  return (lease.ip != 0U);
}

/**
 * @brief Preserve the terminal verdict as a periodic console heartbeat.
 * @details Emits the immutable pass or fail line and sleeps for the fixed
 *          heartbeat period forever after the network journey completes.
 * @param[in] passed Final application verdict.
 * @return Never returns.
 * @pre The console and ThreadX scheduler are operational.
 * @pre No further network setup step is required.
 * @post The selected verdict is never changed.
 * @post Each loop iteration yields through ::tx_thread_sleep.
 * @note Not thread-safe; this consumes the worker thread permanently.
 * @since 0.1.0
 */
static void c6_join_heartbeat(bool passed)
{
  while (true) {
    c6_join_puts(passed ? "c6_join: alive PASS\r\n" : "c6_join: alive FAIL\r\n");
    tx_thread_sleep((ULONG)k_c6_join_heartbeat_ms);
  }
}

/**
 * @brief Run provisioning, C6 readiness, association, DHCP, and heartbeat.
 * @details Refuses a failed port, receives one bounded runtime record, uses it
 *          only for synchronous association, erases it, completes DHCP, and
 *          enters the terminal heartbeat with the aggregate verdict.
 * @param[in] thread_input Unused ThreadX entry argument.
 * @return Nothing on provisioning failure; successful journeys enter heartbeat.
 * @pre ThreadX started this worker after ::tx_application_define.
 * @pre ::s_init_err contains the ESP-hosted initialization result.
 * @post The credential record is erased before DHCP or return.
 * @post No credential-bearing field is written to the console.
 * @note Not thread-safe; this is the application's sole worker thread.
 * @since 0.1.0
 */
static void c6_join_worker_entry(ULONG thread_input)
{
  (void)thread_input;

  c6_join_puts("c6_join: port_init=");
  c6_join_puts(ra8_err_to_str(s_init_err));
  c6_join_puts("\r\n");
  if (s_init_err != k_ra8_ok) {
    c6_join_puts("c6_join: FAIL port init failed -- no transaction attempted\r\n");
    c6_join_heartbeat(false);
    return;
  }

  ra8_delay_ms((uint32_t)k_c6_join_boot_wait_ms);

  ra8_net_credentials_t credentials = {};
  const ra8_err_t provisioned = ra8_net_provision_receive(&s_provision_uart,
                                                          (uint32_t)k_ra8_net_provision_timeout_ms,
                                                          &credentials);
  if (provisioned != k_ra8_ok) {
    c6_join_puts("c6_join: FAIL runtime provisioning err=");
    c6_join_puts(ra8_err_to_str(provisioned));
    c6_join_puts("\r\n");
    ra8_net_provision_clear(&credentials);
    c6_join_heartbeat(false);
    return;
  }

  ra8_c6link_fw_version_t fw  = {};
  ra8_c6link_mac_t        mac = {};
  bool                    ok  = c6_join_phase_ready(&fw);
  ok                          = ok && c6_join_phase_associate(&mac, &credentials);
  ra8_net_provision_clear(&credentials);
  ok = ok && c6_join_phase_ip(&mac);

  c6_join_puts("c6_join: events_seen=");
  c6_join_put_u32((uint32_t)s_events);
  c6_join_puts("\r\n");
  if (ok) {
    c6_join_puts("c6_join: PASS ra8_c6link joined the bench Wi-Fi and DHCP leased an "
                 "address\r\n");
  } else {
    c6_join_puts("c6_join: FAIL Wi-Fi join to a DHCP lease did not complete\r\n");
  }
  c6_join_heartbeat(ok);
}

/**
 * @brief ThreadX define hook: bring the port up and start the worker.
 * @param[in] first_unused_memory Free RAM from the ThreadX port; unused, every
 *                                object here is static.
 * @return Nothing.
 * @pre ``tx_kernel_enter`` has been called and the clock cache is valid.
 * @pre No vendored esp-hosted entry point has been called yet.
 * @post ::s_init_err holds the exact port-init result.
 * @post Exactly one worker thread was created.
 * @note The port creates ThreadX byte pools, mutexes and timers, so its init
 *       must run where ThreadX permits object creation -- here, not from main.
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  const ra8_esp_hosted_port_cfg_t cfg = {
    .pclk_hz      = s_pclka_hz,
    .sck_hz       = (uint32_t)k_c6_join_sck_hz,
    .edge_poll_ms = (uint16_t)k_c6_join_edge_poll_ms,
    .sci_channel  = (uint8_t)k_ra8_board_pmod1_sci_channel,
  };
  s_init_err = ra8_esp_hosted_port_init(&cfg);

  if (tx_thread_create(&s_worker,
                       s_worker_name,
                       c6_join_worker_entry,
                       0U,
                       s_worker_stack,
                       (ULONG)sizeof(s_worker_stack),
                       (UINT)k_c6_join_worker_prio,
                       (UINT)k_c6_join_worker_prio,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    c6_join_puts("c6_join: worker thread creation failed\r\n");
  }
}

/**
 * @brief Application entry: clocks, console, banner, then ThreadX.
 * @pre ``Reset_Handler`` has copied ``.data`` and zeroed ``.bss``.
 * @pre ``SystemInit`` has set VTOR, the FPU and the priority grouping.
 * @post The banner was printed exactly once.
 * @post Control passed to ThreadX and never came back.
 * @note Everything after ``tx_kernel_enter`` runs on ThreadX; the panic-halt
 *       below is reached only if the kernel refuses to start.
 * @since 0.1.0
 */
void main(void)
{
  c6_join_setup_or_halt();
  ra8_isr_globals_enable();

  c6_join_print_banner(s_cpuclk_hz, s_pclka_hz);
  c6_join_puts("c6_join: entering ThreadX; port init runs from tx_application_define\r\n");

  tx_kernel_enter();

  c6_join_panic_halt();
}
