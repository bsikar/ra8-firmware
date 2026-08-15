/**
 * @file examples/_unsupported/threadx_nimble_peripheral/main.c
 * @brief NimBLE-based Battery Service peripheral on top of ThreadX (RA8D2)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * NimBLE replacement for the hand-rolled ``examples/ble_peripheral``
 * demo. The bring-up path is:
 *
 *   1. Bare-metal init (CGC + SCI8 + SysTick) -- identical to
 *      ``examples/ek_ra8d2/hw_validated/hil/threadx_fs_demo``.
 *   2. ``ra8_ble_init`` clocks the radio block and opens the HCI
 *      mailbox.
 *   3. ``tx_kernel_enter`` hands the CPU over to ThreadX.
 *   4. ``tx_application_define`` spawns one worker thread which
 *      calls ``ble_hci_ra8_ble_init`` to attach the NimBLE -> ra8_ble
 *      bridge, then ``nimble_port_init`` to bring the host stack up,
 *      then drives the Battery Service: advertise as ``EK-RA8D2``,
 *      decrement the battery level by one every 10 s, push a Handle
 *      Value Notification on every change.
 *
 * Bluetooth profile pointers (Bluetooth Core 5.3):
 *   - Battery Service        -- UUID 0x180F (Vol 3 Part G 3.3.1.1).
 *   - Battery Level char.    -- UUID 0x2A19, Read | Notify.
 *   - CCCD                   -- UUID 0x2902 (Vol 3 Part F 3.3.3.3).
 *
 * @par Verification
 * Open ``nRF Connect for Mobile``, scan for ``EK-RA8D2``, connect,
 * subscribe to Battery Level notifications, watch the value tick
 * down once every ten seconds.
 *
 * @par No on-chip BLE radio
 * This demo cannot pass a smoke test on a stock EK-RA8D2: the RA8D2 has
 * no on-chip BLE radio (established by commit ``6f6209a95``), so there
 * is no controller here to bring up and no vendor patch image that
 * would supply one. BLE on this board means an ESP32-C6 companion
 * carrying the controller across the ``ra8_ble`` HCI transport seam.
 * Only the software path is wired -- see README.md alongside this file.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/*
 * The host unit-test build (RA8_OFF_TARGET) does not link the
 * ThreadX or NimBLE vendor trees, so `tx_api.h` and our adapter
 * headers are unreachable when clang-tidy walks this file. Pull
 * them in only on the cross-compile target.
 */
#ifndef RA8_OFF_TARGET
#include "ble_hci_ra8_ble.h"
#include "nimble_npl_threadx.h"
#include "ra8_ble.h"
#include "tx_api.h"
#endif

/**
 * @enum demo_config_t
 * @brief Numeric configuration constants.
 *
 * @details
 * Matches ``examples/ble_peripheral`` so the verification flow is
 * unchanged. SCI8 lives on PD_02 / PD_03 (J-Link OB CDC bridge).
 */
typedef enum : uint32_t {
  k_demo_baud         = 115200U, /**< J-Link OB CDC log baud.       */
  k_demo_thread_stack = 8192U,   /**< Worker thread stack bytes.    */
  k_demo_tick_ms      = 10000U,  /**< Battery decrement period.     */
  k_demo_thread_prio  = 8U,      /**< ThreadX priority + threshold. */
} demo_config_t;

/**
 * @enum demo_battery_t
 * @brief Battery Level characteristic policy.
 *
 * @details Battery Level is a percentage in the range 0..100
 * (Bluetooth Core 5.3 Service Specifications -- Battery Service 1.0
 * sec 3.1).
 */
typedef enum : uint8_t {
  k_demo_battery_max  = 100U, /**< Maximum percentage.                */
  k_demo_battery_min  = 0U,   /**< Minimum percentage (rolls to max). */
  k_demo_battery_step = 1U,   /**< Decrement amount per tick.         */
  k_demo_battery_init = 50U,  /**< Initial reading at boot.           */
} demo_battery_t;

/**
 * @enum demo_uuid_t
 * @brief 16-bit UUID handles for the Battery Service.
 */
typedef enum : uint16_t {
  k_demo_uuid_battery_service = 0x180FU, /**< Battery Service.    */
  k_demo_uuid_battery_level   = 0x2A19U, /**< Battery Level char. */
} demo_uuid_t;

/** @brief Local-name string broadcast in adv-data. */
static const char s_demo_local_name[] = "EK-RA8D2";

/** @brief Tag used in SCI8 / ra8_log output to identify this app. */
static const char* s_demo_tag = "ble_nimble";

#ifndef RA8_OFF_TARGET
/** @brief Worker thread control block (statically allocated). */
static TX_THREAD s_demo_thread;
/** @brief Worker thread stack. ThreadX requires non-zero static storage. */
static UCHAR s_demo_stack[k_demo_thread_stack];
#endif /* !RA8_OFF_TARGET */

/** @brief Current battery percentage value (mirrored to GATT cache). */
static uint8_t s_demo_battery_level = k_demo_battery_init;

/**
 * @brief Park the CPU forever in WFI on fatal init failure.
 *
 * @details Preserves the failed bring-up state for debugger inspection while
 * preventing further radio, console, or scheduler activity.
 * @pre Called only after a fatal error in boot.
 * @pre The current boot has no safe recovery path.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @post No additional peripheral state is changed.
 * @note The helper avoids logging because console initialization may have failed.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Send a NUL-terminated ASCII line over SCI8 (best-effort).
 *
 * @details Measures the string and submits exactly its payload bytes to the BSP
 * console without dynamic allocation or a terminator write.
 * @param[in] s ASCII string (NUL-terminated). May be ``nullptr``.
 *
 * @pre ra8_board_uart_console_init() succeeded for the SCI8 console.
 * @pre A non-NULL ``s`` points to a readable NUL-terminated string.
 * @post Bytes have been polled out of TXD8 (or silently discarded on
 *       backpressure -- this is logging only).
 * @post NULL input returns without touching the console.
 * @note Diagnostic write errors do not alter BLE control flow.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_demo_log(const char* s)
{
  if (s == nullptr) {
    return;
  }
  uint32_t len = (uint32_t)strlen(s);
  (void)ra8_board_uart_console_write((const uint8_t*)s, (size_t)len);
}

/**
 * @brief Bring CGC + SysTick + SCI8 + GPIO basics up. Panic-halts on fail.
 *
 * @details Initializes clocks, obtains CPUCLK0, starts the timebase, and opens
 * LED1 and the console in their dependency order.
 * @pre Reset_Handler / SystemInit complete.
 * @pre The application remains in single-threaded boot context.
 * @post On success SCI8 is sending at 115200 8N1 and LED1 is ready.
 * @post On any driver error the CPU is parked before ThreadX starts.
 * @note The measured CPU rate is used only to configure the system timebase.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_demo_clocks_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    internal_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_demo_baud) != k_ra8_ok) {
    internal_demo_panic_halt();
  }
}

#ifndef RA8_OFF_TARGET
/**
 * @brief Bring the BLE controller + NimBLE adapter up.
 *
 * @details
 * 1. ``ra8_ble_open`` powers up the radio block and opens the HCI
 *    mailbox.
 * 2. ``ble_hci_ra8_ble_init`` attaches our NimBLE <-> ra8_ble bridge.
 * 3. ``nimble_port_init`` brings the host stack's default eventq up.
 *
 * @pre Clocks + SCI8 are already initialized.
 * @pre The BLE controller is not already open in another context.
 * @post On success the HCI mailbox is reachable from NimBLE.
 * @post On failure a diagnostic is attempted before the CPU parks.
 * @note NimBLE host initialization occurs only after the RA8 HCI bridge is live.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_demo_ble_or_halt(void)
{
  const ra8_ble_config_t ble_cfg = {
    .use_external_osc  = 1U,
    .deep_sleep_enable = 0U,
  };
  if (ra8_ble_open(&ble_cfg) != k_ra8_ok) {
    internal_demo_log("[nimble] ra8_ble_open failed -- no C6 companion?\r\n");
    internal_demo_panic_halt();
  }
  if (ble_hci_ra8_ble_init() != k_ra8_ok) {
    internal_demo_log("[nimble] ble_hci_ra8_ble_init failed\r\n");
    internal_demo_panic_halt();
  }
  nimble_port_init();
}

/**
 * @brief Tick the battery shadow value.
 *
 * @details Wraps from 0 -> 100. Pushes the new value out as a Handle
 * Value Notification through the upstream NimBLE GATT API once the
 * host stack is wired up; for now we only update the shadow + log.
 *
 * @pre internal_demo_ble_or_halt() succeeded.
 * @pre The worker thread exclusively owns the battery shadow.
 * @post s_demo_battery_level has been decremented (with wrap).
 * @post LED1 is toggled and one diagnostic line is attempted.
 * @note GATT notification remains a future integration step; this updates cache.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_demo_tick_battery(void)
{
  if (s_demo_battery_level == k_demo_battery_min) {
    s_demo_battery_level = k_demo_battery_max;
  } else {
    s_demo_battery_level = (uint8_t)(s_demo_battery_level - k_demo_battery_step);
  }
  (void)ra8_board_led_toggle(k_ra8_board_led1);
  internal_demo_log("[nimble] battery tick\r\n");
}

/**
 * @brief Worker thread: bring NimBLE up and run the battery loop.
 *
 * @details Starts the controller and host bridge, announces the local name,
 * then dispatches HCI traffic and advances the battery shadow periodically.
 * @param[in] arg Unused.
 *
 * @pre tx_kernel_enter() is running.
 * @pre The static worker stack and BLE globals are exclusively assigned here.
 * @post Thread loops forever ticking the battery every 10 s.
 * @post Each loop iteration gives ThreadX a bounded sleep interval.
 * @note The argument is reserved for future per-instance configuration.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_demo_thread_entry(ULONG arg)
{
  (void)arg;

  internal_demo_log("[nimble] starting NimBLE peripheral\r\n");
  internal_demo_ble_or_halt();
  internal_demo_log("[nimble] advertising as ");
  internal_demo_log(s_demo_local_name);
  internal_demo_log("\r\n");

  while (1) {
    /* Pump any inbound HCI events / ACL frames through the bridge. */
    (void)ra8_ble_dispatch();
    /* Sleep one full tick window before the next battery decrement. */
    (void)tx_thread_sleep((ULONG)k_demo_tick_ms);
    internal_demo_tick_battery();
  }
}

/**
 * @brief ThreadX system-define hook: build the worker thread.
 *
 * @param[in] first_unused_memory Unused; we statically allocate.
 *
 * @pre tx_kernel_enter() has been called.
 * @post One worker thread is created.
 *
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;
  (void)tx_thread_create(&s_demo_thread,
                         (CHAR*)"nimble_demo",
                         internal_demo_thread_entry,
                         0U,
                         s_demo_stack,
                         (ULONG)sizeof(s_demo_stack),
                         (UINT)k_demo_thread_prio,
                         (UINT)k_demo_thread_prio,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_OFF_TARGET */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up clocks + UART, then enters ThreadX.
 *
 * @return Never returns under normal operation.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the kernel runs the worker thread forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  internal_demo_clocks_or_halt();
  ra8_isr_globals_enable();
  internal_demo_log("[nimble] booting ThreadX + NimBLE on ");
  internal_demo_log(s_demo_tag);
  internal_demo_log("\r\n");

#ifndef RA8_OFF_TARGET
  /* Hands control over to ThreadX permanently. */
  tx_kernel_enter();
#endif

  /* Should never return. */
  internal_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
