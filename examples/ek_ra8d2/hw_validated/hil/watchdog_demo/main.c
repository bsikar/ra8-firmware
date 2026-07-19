/**
 * @file examples/ek_ra8d2/hw_validated/hil/watchdog_demo/main.c
 * @brief IWDT watchdog + reset-cause demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Two-stage demo:
 *
 *   1. On boot, snapshot the reset cause via ``ra8_reset_init`` +
 *      ``ra8_reset_get_cause`` and log it over SCI8 -- a power-on
 *      reset reads ``power_on``, while a deliberate IWDT trip from
 *      the previous run reads ``iwdt``.
 *   2. Refresh the IWDT counter for ``k_wdt_demo_alive_seconds``
 *      seconds (LED1 toggles each refresh as a heartbeat), then
 *      stop refreshing and let the IWDT underflow trip a hardware
 *      reset. On the next boot the cause flips to ``iwdt`` and the
 *      cycle repeats.
 *
 * The IWDT period itself is configured by the OFS0 option-setting
 * register at flash-write time (the chip cannot be reconfigured at
 * runtime); ``examples/ek_ra8d2/uart_hello/linker_script.ld`` -- the
 * shared template -- sets a multi-second window so the
 * "stop refreshing" stage takes a visible amount of time before the
 * reset fires.
 *
 * Note: in the simulator (host-side test) ``ra8_reset_software_reset``
 * returns; on real silicon it never returns. The app's ``while`` loop
 * is the IWDT-stop stage, terminated only by the chip resetting.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_iwdt.h"
#include "ra8_reset.h"
#include "ra8_time.h"

/** @brief Time-unit conversion. */
typedef enum : uint32_t {
  k_ms_per_sec = 1000U, /**< Milliseconds per second. */
} wdt_demo_time_unit_t;

typedef enum : uint32_t {
  k_wdt_demo_baud          = 115200U, /**< Wdt demo baud.          */
  k_wdt_demo_refresh_ms    = 100U,    /**< Wdt demo refresh ms.    */
  k_wdt_demo_alive_seconds = 30U,     /**< Wdt demo alive seconds. */
} wdt_demo_const_t;

/** @brief Banner emitted when the cause is ``power_on``. */
static const uint8_t k_wdt_demo_msg_pwr[] = "wdt: boot reason=power_on\r\n";
/** @brief Banner emitted when the cause is ``iwdt`` (we tripped). */
static const uint8_t k_wdt_demo_msg_wdt[] = "wdt: boot reason=iwdt\r\n";
/** @brief Banner emitted for any other cause. */
static const uint8_t k_wdt_demo_msg_other[] = "wdt: boot reason=other\r\n";
/** @brief Logged just before the demo stops feeding the IWDT. */
static const uint8_t k_wdt_demo_msg_stop[] = "wdt: stopping refresh, expect reset\r\n";

static void wdt_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Pick the right banner string for ``cause``.
 *
 * @par MC/DC:
 * Compound decision: ``cause == power_on || cause == iwdt`` (default).
 * Two atomic conditions x N+1 = 3 vectors -- power_on (this returns
 * pwr msg), iwdt (returns wdt msg), other cause (returns other msg).
 *
 * @since 0.1.0
 */
static const uint8_t* wdt_demo_banner_for(ra8_reset_cause_t cause, uint32_t* out_len)
{
  if (cause == k_ra8_reset_cause_power_on) {
    *out_len = (uint32_t)(sizeof(k_wdt_demo_msg_pwr) - 1U);
    return k_wdt_demo_msg_pwr;
  }
  if (cause == k_ra8_reset_cause_iwdt) {
    *out_len = (uint32_t)(sizeof(k_wdt_demo_msg_wdt) - 1U);
    return k_wdt_demo_msg_wdt;
  }
  *out_len = (uint32_t)(sizeof(k_wdt_demo_msg_other) - 1U);
  return k_wdt_demo_msg_other;
}

static void wdt_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    wdt_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    wdt_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    wdt_demo_panic_halt();
  }
  if (ra8_reset_init() != k_ra8_ok) {
    wdt_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_wdt_demo_baud) != k_ra8_ok) {
    wdt_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    wdt_demo_panic_halt();
  }
  if (ra8_iwdt_init() != k_ra8_ok) {
    wdt_demo_panic_halt();
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  wdt_demo_setup_or_halt();
  ra8_isr_globals_enable();

  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;
  (void)ra8_reset_get_cause(&cause);
  uint32_t       msg_len = 0U;
  const uint8_t* msg     = wdt_demo_banner_for(cause, &msg_len);
  (void)ra8_board_uart_console_write(msg, (size_t)msg_len);

  /* Stage 1: refresh for ``alive_seconds`` seconds. */
  const uint32_t refreshes_per_sec = k_ms_per_sec / (uint32_t)k_wdt_demo_refresh_ms;
  const uint32_t total_refreshes   = (uint32_t)k_wdt_demo_alive_seconds * refreshes_per_sec;
  for (uint32_t i = 0U; i < total_refreshes; ++i) {
    ra8_iwdt_refresh_deferred();
    (void)ra8_board_led_toggle(k_ra8_board_led1);
    ra8_delay_ms((uint32_t)k_wdt_demo_refresh_ms);
  }

  /* Stage 2: stop refreshing and let the IWDT underflow reset us. */
  (void)ra8_board_uart_console_write(k_wdt_demo_msg_stop,
                                     (size_t)(sizeof(k_wdt_demo_msg_stop) - 1U));
  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
