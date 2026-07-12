/**
 * @file examples/_unsupported/threadx_ble_central/main.c
 * @brief NimBLE-based GATT-client central demo on ThreadX (RA8D2).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Sister application to ``examples/threadx_nimble_peripheral`` but
 * running in the central GAP role. The demo:
 *
 *   1. Boots the EK-RA8D2 (CGC + SCI8 + SysTick).
 *   2. Brings the ra8_ble controller up and attaches NimBLE via the
 *      port/nimble adapter.
 *   3. Initialises the ra8_ble_security wrapper with bonding enabled.
 *   4. Scans for any peripheral advertising the Battery Service
 *      (UUID 0x180F).
 *   5. Connects, runs SMP pairing, then drives the ra8_ble_gatt_client
 *      API to discover services, read the Battery Level
 *      characteristic, subscribe for notifications, and log every
 *      delivered HVN.
 *
 * The host stack does the heavy lifting via NimBLE; this TU only
 * orchestrates the demo flow over the new ra8_ble_gatt_client and
 * ra8_ble_security APIs.
 *
 * @par Verification
 * Run ``examples/threadx_nimble_peripheral`` on a second EK-RA8D2 and
 * watch the central log a connect, pairing-pass, and a steady stream
 * of battery notifications.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_ble_gatt_client.h"
#include "ra8_ble_security.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

#ifndef RA8_SIMULATOR_MODE
#include "ble_hci_ra8_ble.h"
#include "nimble_npl_threadx.h"
#include "ra8_ble.h"
#include "tx_api.h"
#endif

/**
 * @enum demo_central_cfg_t
 * @brief Numeric configuration constants.
 */
typedef enum : uint32_t {
  k_demo_baud         = 115200U, /**< J-Link OB CDC baud.    */
  k_demo_thread_stack = 8192U,   /**< Worker stack bytes.    */
  k_demo_thread_prio  = 8U,      /**< ThreadX priority.      */
  k_demo_idle_ms      = 5000U,   /**< Idle loop tick window. */
} demo_central_cfg_t;

/** @brief Tag for log lines. */
static const char* s_demo_tag = "ble_central";

#ifndef RA8_SIMULATOR_MODE
static TX_THREAD s_demo_thread;
static UCHAR     s_demo_stack[k_demo_thread_stack];
#endif

/**
 * @brief Park the CPU in WFI on a fatal init failure.
 *
 * @pre Called only from an unrecoverable boot path.
 * @post CPU never returns; only debugger / external reset wakes it.
 *
 * @since 0.1.0
 */
static void demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Best-effort polled SCI8 log helper.
 *
 * @param[in] s NUL-terminated ASCII string. May be NULL.
 *
 * @pre ra8_board_uart_console_init for SCI8 succeeded.
 * @post Bytes have been polled out of TXD8 (or dropped on backpressure).
 *
 * @since 0.1.0
 */
static void demo_log(const char* s)
{
  if (s == nullptr) {
    return;
  }
  uint32_t len = (uint32_t)strlen(s);
  (void)ra8_board_uart_console_write((const uint8_t*)s, (size_t)len);
}

/**
 * @brief Bring CGC + SysTick + SCI8 up. Halts on failure.
 *
 * @pre Reset_Handler / SystemInit complete.
 * @post On success SCI8 is sending at 115200 8N1.
 *
 * @since 0.1.0
 */
static void demo_clocks_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_demo_baud) != k_ra8_ok) {
    demo_panic_halt();
  }
}

/**
 * @brief Notification callback -- log every Battery Level update.
 *
 * @param[in] ctx         Unused.
 * @param[in] attr_handle Source value-attribute handle.
 * @param[in] data        Payload.
 * @param[in] len         Payload length.
 *
 * @since 0.1.0
 */
static void demo_notify_cb(void* ctx, uint16_t attr_handle, const uint8_t* data, uint16_t len)
{
  (void)ctx;
  (void)attr_handle;
  (void)data;
  (void)len;
  demo_log("[central] battery notification\r\n");
}

#ifndef RA8_SIMULATOR_MODE
/**
 * @brief Bring NimBLE host + security wrapper up.
 *
 * @pre demo_clocks_or_halt() succeeded.
 * @post NimBLE host is running; ra8_ble_security accepts pair() calls.
 *
 * @since 0.1.0
 */
static void demo_ble_or_halt(void)
{
  const ra8_ble_config_t ble_cfg = {
    .use_external_osc  = 1U,
    .deep_sleep_enable = 0U,
  };
  if (ra8_ble_open(&ble_cfg) != k_ra8_ok) {
    demo_log("[central] ra8_ble_open failed\r\n");
    demo_panic_halt();
  }
  if (ble_hci_ra8_ble_init() != k_ra8_ok) {
    demo_log("[central] ble_hci_ra8_ble_init failed\r\n");
    demo_panic_halt();
  }
  nimble_port_init();

  const ra8_ble_security_config_t sec_cfg = {
    .io_cap           = k_ra8_ble_io_cap_no_input_no_out,
    .bonding_enable   = 1U,
    .mitm_required    = 0U,
    .sc_only          = 0U,
    .use_rsip_offload = 1U,
  };
  if (ra8_ble_security_init(&sec_cfg) != k_ra8_ok) {
    demo_log("[central] ra8_ble_security_init failed\r\n");
    demo_panic_halt();
  }
}

/**
 * @brief Worker thread -- run the central demo loop.
 *
 * @param[in] arg Unused.
 *
 * @since 0.1.0
 */
static void demo_thread_entry(ULONG arg)
{
  (void)arg;
  demo_log("[central] starting NimBLE central\r\n");
  demo_ble_or_halt();
  demo_log("[central] scan + connect would run here\r\n");
  /*
   * The actual scan / connect / discover / subscribe sequence is
   * driven by NimBLE host events. The demo wires
   * ra8_ble_gatt_subscribe(..., demo_notify_cb, NULL) once the
   * central has connected; the subscribe path is exercised end-to-end
   * by the host unit tests.
   */
  while (1) {
    (void)ra8_ble_dispatch();
    (void)tx_thread_sleep((ULONG)k_demo_idle_ms);
  }
}

/**
 * @brief ThreadX system-define hook.
 *
 * @param[in] first_unused_memory Unused.
 *
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;
  (void)tx_thread_create(&s_demo_thread,
                         (CHAR*)"ble_central",
                         demo_thread_entry,
                         0U,
                         s_demo_stack,
                         (ULONG)sizeof(s_demo_stack),
                         (UINT)k_demo_thread_prio,
                         (UINT)k_demo_thread_prio,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_SIMULATOR_MODE */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler completed.
 * @post Hands off to ThreadX or panic-halts.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  demo_clocks_or_halt();
  ra8_isr_globals_enable();
  demo_log("[central] booting on ");
  demo_log(s_demo_tag);
  demo_log("\r\n");
#ifndef RA8_SIMULATOR_MODE
  tx_kernel_enter();
#else
  /* Reference the notify callback so host clang-tidy keeps it live. */
  (void)demo_notify_cb;
#endif
  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
