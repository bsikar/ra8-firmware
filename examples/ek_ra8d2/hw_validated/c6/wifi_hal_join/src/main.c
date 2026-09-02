/**
 * @file examples/ek_ra8d2/hw_validated/c6/wifi_hal_join/src/main.c
 * @brief The same Wi-Fi join as c6_wifi_join, the HAL way, through ra8_wifi.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * A deliberate side-by-side with ``c6_wifi_join`` (kept unchanged next door):
 * both associate the ESP32-C6 station with the bench access point and obtain a
 * DHCP lease, but this one does it through the ``ra8_wifi`` facade. The whole
 * network journey is six calls -- ::ra8_wifi_c6link_setup, ::ra8_wifi_init,
 * ::ra8_wifi_connect, ::ra8_wifi_wait_ip, plus ::ra8_wifi_get_mac and
 * ::ra8_wifi_get_ap for the printout -- with no ``ra8_c6link`` handle poked, no
 * RPC sequence, no event callback and no station config struct in sight. The
 * one thing the facade cannot own, obtaining an IP, is a caller-supplied
 * provider: ::wifi_hal_ip_bind wraps NetX Duo's DHCP client.
 *
 * The credential-free image accepts one bounded runtime provisioning record
 * over the board console and erases it after the synchronous network journey.
 *
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "nx_ether_driver_c6.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_c6link.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_c6link.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_net_provision.h"
#include "ra8_time.h"
#include "ra8_wifi.h"
#include "ra8_wifi_c6link.h"
#include "tx_api.h"
#include "wifi_hal_join.h"

/** @brief Cached CPUCLK0 rate. @since 0.1.0 */
static uint32_t s_cpuclk_hz;
/** @brief Cached PCLKA rate. @since 0.1.0 */
static uint32_t s_pclka_hz;
/** @brief Port-init result from tx_application_define. @since 0.1.0 */
static ra8_err_t s_init_err = k_ra8_err_not_initialized;
/** @brief Application worker thread. @since 0.1.0 */
static TX_THREAD s_worker;
/** @brief Worker thread name. @since 0.1.0 */
static CHAR s_worker_name[] = "wifi_hal_join";
/** @brief Worker thread stack. @since 0.1.0 */
static UCHAR s_worker_stack[k_wifi_hal_worker_stack];
/** @brief Decode arena handed to the backend. @since 0.1.0 */
static uint8_t s_arena[k_wifi_hal_arena_bytes];
/** @brief The one C6 link handle this app owns. @since 0.1.0 */
static ra8_c6link_t s_link;
/** @brief The ESP32-C6 ra8_wifi backend context. @since 0.1.0 */
static ra8_wifi_c6link_t s_c6;
/** @brief The Wi-Fi handle every operation goes through. @since 0.1.0 */
static ra8_wifi_t s_wifi;
/** @brief Board-console binding for the shared runtime provisioner. @since 0.1.0 */
static const ra8_net_provision_uart_t s_provision_uart = {
  .write   = ra8_board_uart_console_write,
  .read    = ra8_board_uart_console_read,
  .wait_ms = ra8_delay_ms,
};

/**
 * @brief Halt the processor after an unrecoverable start-up failure.
 * @details Executes wait-for-interrupt forever so failed platform or kernel
 *          initialization cannot fall through into a partial network journey.
 * @return Never returns.
 * @pre A terminal start-up failure was detected.
 * @pre Interrupt state is safe for an indefinite wait.
 * @post No application operation executes after entry.
 * @post The processor remains in the bounded halt loop.
 * @note Not thread-safe; this is a terminal single-context path.
 * @since 0.1.0
 */
static void wifi_hal_panic_halt(void)
{
  while (true) {
    __asm volatile("wfi");
  }
}

/**
 * @brief Initialize platform services required by the HAL join application.
 * @details Brings up clocks, caches live rates, enables module-stop control,
 *          initializes delay timing, and opens the console in dependency order.
 * @return Nothing after successful initialization; failures halt permanently.
 * @pre Reset-time clock and board state is available.
 * @pre No worker or Wi-Fi operation has started.
 * @post Live CPUCLK0 and PCLKA rates are cached.
 * @post Timing and board-console services are initialized on return.
 * @note Not thread-safe; call exactly once during boot.
 * @since 0.1.0
 */
static void wifi_hal_setup_or_halt(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    wifi_hal_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &s_cpuclk_hz) != k_ra8_ok) {
    wifi_hal_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &s_pclka_hz) != k_ra8_ok) {
    wifi_hal_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    wifi_hal_panic_halt();
  }
  if (ra8_time_init(s_cpuclk_hz) != k_ra8_ok) {
    wifi_hal_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_wifi_hal_uart_baud) != k_ra8_ok) {
    wifi_hal_panic_halt();
  }
}

/**
 * @brief Build the C6-backed Wi-Fi and NetX DHCP configuration.
 * @details Binds ESP-hosted transport into the fixed-arena C6 backend, connects
 *          that backend to the facade configuration, and installs DHCP binding.
 * @param[out] out Wi-Fi facade configuration destination.
 * @return Repository error code.
 * @retval k_ra8_ok The complete configuration was bound.
 * @retval other Transport binding or backend setup error propagated unchanged.
 * @pre `out` is non-null and writable.
 * @pre ESP-hosted platform initialization completed successfully.
 * @post Success initializes the backend, DHCP callback, and callback context.
 * @post Failure leaves no active Wi-Fi journey.
 * @note Not thread-safe; called once by the worker.
 * @since 0.1.0
 */
static ra8_err_t wifi_hal_make_cfg(ra8_wifi_cfg_t* out)
{
  ra8_wifi_c6link_cfg_t bcfg = {};
  bcfg.link                  = &s_link;
  bcfg.arena                 = s_arena;
  bcfg.arena_bytes           = (uint32_t)sizeof(s_arena);
  bcfg.rx_cb                 = nx_ether_driver_c6_rx;
  const ra8_err_t bound      = ra8_esp_hosted_c6link_bind(&bcfg.transport);
  if (bound != k_ra8_ok) {
    return bound;
  }
  const ra8_err_t wired = ra8_wifi_c6link_setup(&s_c6, &bcfg, out);
  if (wired != k_ra8_ok) {
    return wired;
  }
  out->ip_bind = wifi_hal_ip_bind;
  out->ip_ctx  = &s_link;
  return k_ra8_ok;
}

/**
 * @brief Print the non-secret fields of one DHCP lease.
 * @details Emits address, mask, gateway, and DHCP server in dotted-quad form
 *          through bounded console helpers.
 * @param[in] lease Bound lease record to print.
 * @return Nothing.
 * @pre `lease` is non-null and initialized.
 * @pre The board console is initialized.
 * @post Every lease address field is emitted once.
 * @post No credential-bearing data is emitted.
 * @note Not thread-safe; the worker is the sole caller.
 * @since 0.1.0
 */
static void wifi_hal_print_lease(const ra8_wifi_lease_t* lease)
{
  wifi_hal_puts("wifi_hal: dhcp bound ip=");
  wifi_hal_put_ip(lease->ip);
  wifi_hal_puts(" mask=");
  wifi_hal_put_ip(lease->mask);
  wifi_hal_puts(" gw=");
  wifi_hal_put_ip(lease->gateway);
  wifi_hal_puts(" server=");
  wifi_hal_put_ip(lease->dhcp_server);
  wifi_hal_puts("\r\n");
}

static bool wifi_hal_report(const wifi_hal_result_t* res, bool joined);

/**
 * @brief Execute the HAL network journey with runtime credentials.
 * @details Builds the hardware configuration, passes the caller-owned runtime
 *          strings into the host-tested orchestration, and reports its result.
 * @param[in] credentials Validated runtime SSID and PSK record.
 * @return Journey outcome.
 * @retval true Association and DHCP completed.
 * @retval false Hardware binding, initialization, association, or DHCP failed.
 * @pre `credentials` is non-null and contains validated fields.
 * @pre ESP-hosted initialization completed successfully.
 * @post The shared journey has synchronously consumed the credential strings.
 * @post No credential-bearing field is written to the console.
 * @note Not thread-safe; called once by the worker.
 * @since 0.1.0
 */
static bool wifi_hal_run(const ra8_net_credentials_t* credentials)
{
  ra8_wifi_cfg_t  cfg  = {};
  const ra8_err_t made = wifi_hal_make_cfg(&cfg);
  if (made != k_ra8_ok) {
    wifi_hal_puts("wifi_hal: FAIL transport setup err=");
    wifi_hal_puts(ra8_err_to_str(made));
    wifi_hal_puts("\r\n");
    return false;
  }

  const wifi_hal_run_cfg_t rcfg = {
    .wifi_cfg    = &cfg,
    .wifi        = &s_wifi,
    .ssid        = credentials->ssid,
    .psk         = credentials->psk,
    .poll_budget = (uint32_t)k_wifi_hal_assoc_polls,
  };
  wifi_hal_result_t res    = {};
  const bool        joined = wifi_hal_join_run(&rcfg, &res);
  return wifi_hal_report(&res, joined);
}

/**
 * @brief Report one completed HAL journey and derive its terminal verdict.
 * @details Emits each non-secret stage result and lease while requiring the
 *          aggregate result to agree with its recorded successful stages.
 * @param[in] res Completed join result record.
 * @param[in] joined Aggregate verdict returned by ::wifi_hal_join_run.
 * @return Final reported verdict.
 * @retval true Every stage and the aggregate verdict succeeded.
 * @retval false Initialization, association, DHCP, or aggregate verdict failed.
 * @pre `res` is non-null and initialized by ::wifi_hal_join_run.
 * @pre The console is initialized.
 * @post A failed stage emits one non-secret failure line.
 * @post Success emits association and lease information without SSID or PSK.
 * @note Not thread-safe; the worker is the sole caller.
 * @since 0.1.0
 */
static bool wifi_hal_report(const wifi_hal_result_t* res, bool joined)
{
  wifi_hal_puts("wifi_hal: init=");
  wifi_hal_puts(ra8_err_to_str(res->init_err));
  wifi_hal_puts("\r\n");
  if (!res->init_ok) {
    return false;
  }

  if (!res->associated) {
    wifi_hal_puts("wifi_hal: FAIL association did not complete connect=");
    wifi_hal_puts(ra8_err_to_str(res->connect_err));
    wifi_hal_puts("\r\n");
    return false;
  }

  wifi_hal_puts("wifi_hal: associated mac=");
  wifi_hal_put_mac(&res->mac);
  wifi_hal_puts(" chan=");
  wifi_hal_put_u32((uint32_t)res->ap.channel);
  wifi_hal_puts("\r\n");
  if (!res->ip_bound) {
    wifi_hal_puts("wifi_hal: FAIL dhcp did not bind err=");
    wifi_hal_puts(ra8_err_to_str(res->ip_err));
    wifi_hal_puts("\r\n");
    return false;
  }
  wifi_hal_print_lease(&res->lease);
  if (!joined) {
    wifi_hal_puts("wifi_hal: FAIL join_run failed with every stage reported ok\r\n");
  }
  return joined;
}

/**
 * @brief Preserve the terminal verdict as a periodic console heartbeat.
 * @details Emits the immutable pass or fail line and sleeps for the fixed
 *          heartbeat period forever after the Wi-Fi journey completes.
 * @param[in] passed Final application verdict.
 * @return Never returns.
 * @pre The console and ThreadX scheduler are operational.
 * @pre No further network setup step is required.
 * @post The selected verdict is never changed.
 * @post Each loop iteration yields through ::tx_thread_sleep.
 * @note Not thread-safe; this consumes the worker thread permanently.
 * @since 0.1.0
 */
static void wifi_hal_heartbeat(bool passed)
{
  while (true) {
    wifi_hal_puts(passed ? "wifi_hal: alive PASS\r\n" : "wifi_hal: alive FAIL\r\n");
    tx_thread_sleep((ULONG)k_wifi_hal_heartbeat_ms);
  }
}

/**
 * @brief Run runtime provisioning, the HAL journey, erasure, and heartbeat.
 * @details Refuses a failed port, receives one bounded record, passes it through
 *          the synchronous HAL journey, erases it, and enters terminal heartbeat.
 * @param[in] thread_input Unused ThreadX entry argument.
 * @return Nothing on provisioning failure; successful receives enter heartbeat.
 * @pre ThreadX started this worker after ::tx_application_define.
 * @pre ::s_init_err contains the ESP-hosted initialization result.
 * @post The credential record is erased before return or heartbeat.
 * @post No credential-bearing field is written to the console.
 * @note Not thread-safe; this is the application's sole worker thread.
 * @since 0.1.0
 */
static void wifi_hal_worker_entry(ULONG thread_input)
{
  (void)thread_input;

  wifi_hal_puts("wifi_hal: port_init=");
  wifi_hal_puts(ra8_err_to_str(s_init_err));
  wifi_hal_puts("\r\n");
  if (s_init_err != k_ra8_ok) {
    wifi_hal_puts("wifi_hal: FAIL port init failed -- no transaction attempted\r\n");
    wifi_hal_heartbeat(false);
    return;
  }

  ra8_delay_ms((uint32_t)k_wifi_hal_boot_wait_ms);

  ra8_net_credentials_t credentials = {};
  const ra8_err_t provisioned = ra8_net_provision_receive(&s_provision_uart,
                                                          (uint32_t)k_ra8_net_provision_timeout_ms,
                                                          &credentials);
  if (provisioned != k_ra8_ok) {
    wifi_hal_puts("wifi_hal: FAIL runtime provisioning err=");
    wifi_hal_puts(ra8_err_to_str(provisioned));
    wifi_hal_puts("\r\n");
    ra8_net_provision_clear(&credentials);
    wifi_hal_puts(k_wifi_hal_fail_line);
    wifi_hal_heartbeat(false);
    return;
  }

  const bool ok = wifi_hal_run(&credentials);
  ra8_net_provision_clear(&credentials);
  wifi_hal_puts(ok ? k_wifi_hal_pass_line : k_wifi_hal_fail_line);
  wifi_hal_heartbeat(ok);
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
 * @note The port creates ThreadX objects, so its init must run where ThreadX
 *       permits object creation -- here, not from main.
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  const ra8_esp_hosted_port_cfg_t cfg = {
    .pclk_hz      = s_pclka_hz,
    .sck_hz       = (uint32_t)k_wifi_hal_sck_hz,
    .edge_poll_ms = (uint16_t)k_wifi_hal_edge_poll_ms,
    .sci_channel  = (uint8_t)k_ra8_board_pmod1_sci_channel,
  };
  s_init_err = ra8_esp_hosted_port_init(&cfg);

  if (tx_thread_create(&s_worker,
                       s_worker_name,
                       wifi_hal_worker_entry,
                       0U,
                       s_worker_stack,
                       (ULONG)sizeof(s_worker_stack),
                       (UINT)k_wifi_hal_worker_prio,
                       (UINT)k_wifi_hal_worker_prio,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    wifi_hal_puts("wifi_hal: worker thread creation failed\r\n");
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
  wifi_hal_setup_or_halt();
  ra8_isr_globals_enable();

  wifi_hal_print_banner(s_cpuclk_hz, s_pclka_hz);
  wifi_hal_puts("wifi_hal: entering ThreadX; port init runs from tx_application_define\r\n");

  tx_kernel_enter();

  wifi_hal_panic_halt();
}
